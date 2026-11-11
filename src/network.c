#include "network.h"
#include "tls.h"
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/net/http.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_PATH "ux0:data/IPTVita/online.m3u"
#define PREFETCH_PATH "ux0:data/IPTVita/prefetch.ts"
#define PACKAGED_CA_PATH "app0:cacert.pem"
#define MAX_BYTES (6u*1024u*1024u)
static char url_copy[8192];
static volatile int state;
static volatile int curl_error;
static volatile int http_status;
static int network_ready;
static void *net_memory;
/* A single speculative download uses its own file and URL, so it cannot
 * race with the foreground playlist/segment worker's shared online.m3u. */
static char prefetch_url[8192];
static volatile int prefetch_state;
static volatile unsigned prefetch_generation;
static unsigned prefetch_hits;
typedef struct {SceUID fd;size_t total;int failed;} Download;
static size_t write_file(char *data,size_t size,size_t nmemb,void *userdata)
{
 Download *d=(Download*)userdata;
 size_t n=size*nmemb;
 if(size && n/size!=nmemb) return 0;
 if(n>MAX_BYTES-d->total) {d->failed=1;return 0;}
 int written=sceIoWrite(d->fd,data,(unsigned int)n);
 if(written<0 || (size_t)written!=n) {d->failed=2;return 0;}
 d->total+=n;return n;
}
static int init_network(void)
{
 if(network_ready) return 0;
 if(sceSysmoduleLoadModule(SCE_SYSMODULE_NET)<0) return -1;
 SceNetInitParam p;
 net_memory=malloc(1024*1024);
 if(!net_memory)return -1;
 p.memory=net_memory;p.size=1024*1024;p.flags=0;
 if(sceNetInit(&p)<0) return -1;
 if(sceNetCtlInit()<0)return -1;
 if(sceSysmoduleLoadModule(SCE_SYSMODULE_HTTP)<0)return -1;
 if(sceSysmoduleLoadModule(SCE_SYSMODULE_SSL)<0)return -1;
 if(sceHttpInit(1024*1024)<0)return -1;
 if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return -1;
 network_ready=1;
 return 0;
}
/* Load PEM with Vita I/O, then let libcurl pass it to its TLS backend
 * as an in-memory certificate bundle (no fopen on Vita device paths). */
static struct curl_blob ca_blob;
static void *ca_memory;
static int prepare_ca_bundle(void)
{
#if LIBCURL_VERSION_NUM < 0x074D00
 return -4; /* CURLOPT_CAINFO_BLOB requires libcurl 7.77.0 */
#else
 if(ca_memory) return 0;
 SceUID fd=sceIoOpen(PACKAGED_CA_PATH,SCE_O_RDONLY,0);
 if(fd<0) return -1;
 SceOff size=sceIoLseek(fd,0,SCE_SEEK_END);
 if(size<1024 || size>1024*1024 || sceIoLseek(fd,0,SCE_SEEK_SET)<0) {
  sceIoClose(fd);return -2;
 }
 void *data=malloc((size_t)size);
 if(!data) {sceIoClose(fd);return -3;}
 size_t used=0;
 while(used<(size_t)size) {
  int n=sceIoRead(fd,(char*)data+used,(unsigned int)((size_t)size-used));
  if(n<=0) break;
  used+=(size_t)n;
 }
 sceIoClose(fd);
 if(used!=(size_t)size) {free(data);return -3;}
 ca_memory=data;
 ca_blob.data=data;
 ca_blob.len=used;
 ca_blob.flags=CURL_BLOB_NOCOPY;
 return 0;
#endif
}
static int download_thread(SceSize args,void *argp)
{
 (void)args;(void)argp;
 int result=-1;
 if(init_network()<0) {result=-8;goto finish;}
 sceIoMkdir("ux0:data/IPTVita",0777);
 if(strncmp(url_copy,"https://",8)==0) {
  /* Separate mbedTLS transport; keep the previous cURL path for plain HTTP. */
  SceUID certfd=sceIoOpen(PACKAGED_CA_PATH,SCE_O_RDONLY,0);
  if(certfd<0){result=-11;goto finish;}
  SceOff clen=sceIoLseek(certfd,0,SCE_SEEK_END);
  if(clen<=0 || clen>1024*1024 || sceIoLseek(certfd,0,SCE_SEEK_SET)<0){
   sceIoClose(certfd);result=-11;goto finish;
  }
  unsigned char *cert=malloc((size_t)clen+1);
  unsigned char *body=malloc(MAX_BYTES+1);
  if(!cert || !body){free(cert);free(body);sceIoClose(certfd);result=-1;goto finish;}
  int nr=sceIoRead(certfd,cert,(unsigned int)clen);
  sceIoClose(certfd);
  if(nr!=(int)clen){free(cert);free(body);result=-11;goto finish;}
  cert[clen]=0;
  int status=0;
  int bytes=iptvita_tls_get(url_copy,cert,(size_t)clen+1,body,MAX_BYTES,&status);
  free(cert);
  http_status=status;
  curl_error=bytes<0?bytes:0;
  if(bytes>0) {
   SceUID output=sceIoOpen(CACHE_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
   if(output>=0) {
    int nw=sceIoWrite(output,body,(unsigned int)bytes);
    sceIoClose(output);
    result=nw==bytes?2:-9;
   }else result=-9;
  }else result=-1;
  free(body);
  if(result!=2)sceIoRemove(CACHE_PATH);
  goto finish;
 }
 if(prepare_ca_bundle()<0) {result=-11;goto finish;}
 SceUID fd=sceIoOpen(CACHE_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
 if(fd<0) {result=-9;goto finish;}
 CURL *curl=curl_easy_init();
 if(!curl) {sceIoClose(fd);result=-10;goto finish;}
 Download d={fd,0,0};
 curl_easy_setopt(curl,CURLOPT_URL,url_copy);
 curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
 curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
 curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,12L);
 curl_easy_setopt(curl,CURLOPT_TIMEOUT,50L);
 curl_easy_setopt(curl,CURLOPT_USERAGENT,"IPTVita/0.6.0");
#if LIBCURL_VERSION_NUM >= 0x074D00
 CURLcode ca_option=curl_easy_setopt(curl,CURLOPT_CAINFO_BLOB,&ca_blob);
 if(ca_option!=CURLE_OK) {
  curl_error=(int)ca_option;
  curl_easy_cleanup(curl);sceIoClose(fd);result=-12;goto finish;
 }
#endif
 curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_file);
 curl_easy_setopt(curl,CURLOPT_WRITEDATA,&d);
 curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
 curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
 curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
 CURLcode code=curl_easy_perform(curl);
 long status=0;
 curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
 curl_error=(int)code;
 http_status=(int)status;
 if(d.failed==1)result=-3;
 else if(code!=CURLE_OK)result=-1;
 else if(status<200||status>=300)result=-2;
 else if(!d.total)result=-2;
 else result=2;
 curl_easy_cleanup(curl);
 sceIoClose(fd);
 if(result!=2)sceIoRemove(CACHE_PATH);
finish:
 state=result;
 sceKernelExitDeleteThread(0);
 return 0;
}
/* Check for an exact URL match, including CDN query tokens. A stale or
 * incomplete cache entry must never replace a foreground download. */
static int copy_prefetched_segment(void)
{
 SceUID src=sceIoOpen(PREFETCH_PATH,SCE_O_RDONLY,0);
 if(src<0)return -1;
 SceOff size=sceIoLseek(src,0,SCE_SEEK_END);
 if(size<=0 || size>MAX_BYTES || size%188!=0 ||
    sceIoLseek(src,0,SCE_SEEK_SET)<0) {
  sceIoClose(src);return -1;
 }
 SceUID dst=sceIoOpen(CACHE_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
 if(dst<0){sceIoClose(src);return -1;}
 unsigned char chunk[188*32];
 SceOff copied=0;
 int ok=1;
 while(copied<size) {
  unsigned amount=(unsigned)((size-copied)>(SceOff)sizeof(chunk)?sizeof(chunk):size-copied);
  int got=sceIoRead(src,chunk,amount);
  if(got<=0 || sceIoWrite(dst,chunk,(unsigned)got)!=got){ok=0;break;}
  copied+=got;
 }
 sceIoClose(src);sceIoClose(dst);
 if(!ok || copied!=size){sceIoRemove(CACHE_PATH);return -1;}
 return 0;
}
int iptvita_network_start(const char *url)
{
 if(state==1 || !url)return -1;
 if(prefetch_state==2 && strcmp(url,prefetch_url)==0) {
  if(copy_prefetched_segment()==0) {
   prefetch_state=0;
   prefetch_url[0]=0;
   ++prefetch_hits;
   state=2;curl_error=0;http_status=200;
   return 0;
  }
  prefetch_state=0;
 }
 size_t n=strlen(url);
 if(n<8 || n>=sizeof(url_copy) ||
   (strncmp(url,"https://",8)!=0 && strncmp(url,"http://",7)!=0))return -1;
 memcpy(url_copy,url,n+1);
 curl_error=0;
 http_status=0;
 state=1;
 SceUID thread=sceKernelCreateThread("iptvita_http",download_thread,0x10000100,128*1024,0,0,NULL);
 if(thread<0) {state=-1;return -1;}
 if(sceKernelStartThread(thread,0,NULL)<0) {sceKernelDeleteThread(thread);state=-1;return -1;}
 return 0;
}
int iptvita_network_state(void) {return state;}
const char *iptvita_network_cache_path(void) {return CACHE_PATH;}
void iptvita_network_reset(void) {if(state!=1)state=0;}

int iptvita_network_error_code(void) {return curl_error;}
int iptvita_network_http_status(void) {return http_status;}

/* Speculative one-segment cache: unlike the foreground downloader, this
 * worker always writes PREFETCH_PATH. It uses a bounded response and verifies
 * the TS packet layout before publishing the slot as ready. */
static int prefetch_worker(SceSize args,void *argp)
{
 (void)args;(void)argp;
 unsigned epoch=prefetch_generation;
 char url[8192];
 snprintf(url,sizeof(url),"%s",prefetch_url);
 int ok=0;
 sceIoMkdir("ux0:data/IPTVita",0777);
 if(strncmp(url,"https://",8)==0) {
  SceUID cf=sceIoOpen(PACKAGED_CA_PATH,SCE_O_RDONLY,0);
  if(cf>=0) {
   SceOff len=sceIoLseek(cf,0,SCE_SEEK_END);
   if(len>0 && len<=1024*1024 && sceIoLseek(cf,0,SCE_SEEK_SET)>=0) {
    unsigned char *cert=(unsigned char*)malloc((size_t)len+1);
    unsigned char *body=(unsigned char*)malloc(MAX_BYTES+1);
    if(cert && body && sceIoRead(cf,cert,(unsigned)len)==len) {
     cert[len]=0;
     int http=0;
     int n=iptvita_tls_get(url,cert,(size_t)len+1,body,MAX_BYTES,&http);
     if(n>0 && n%188==0 && body[0]==0x47 && http==200) {
      SceUID fd=sceIoOpen(PREFETCH_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
      if(fd>=0) {
       ok=sceIoWrite(fd,body,(unsigned)n)==n;
       sceIoClose(fd);
      }
     }
    }
    free(cert);free(body);
   }
   sceIoClose(cf);
  }
 }else if(strncmp(url,"http://",7)==0 && prepare_ca_bundle()==0) {
  SceUID fd=sceIoOpen(PREFETCH_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
  if(fd>=0) {
   CURL *curl=curl_easy_init();
   if(curl) {
    Download d={fd,0,0};
    curl_easy_setopt(curl,CURLOPT_URL,url);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,12L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,35L);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"IPTVita/0.6.0");
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_file);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&d);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
#if LIBCURL_VERSION_NUM >= 0x074D00
    curl_easy_setopt(curl,CURLOPT_CAINFO_BLOB,&ca_blob);
#endif
    CURLcode rc=curl_easy_perform(curl);
    long status=0;
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    ok=rc==CURLE_OK && status>=200 && status<300 && !d.failed &&
       d.total>0 && d.total%188==0;
    curl_easy_cleanup(curl);
   }
   sceIoClose(fd);
   if(ok) {
    SceUID verify=sceIoOpen(PREFETCH_PATH,SCE_O_RDONLY,0);
    unsigned char b=0;
    ok=verify>=0 && sceIoRead(verify,&b,1)==1 && b==0x47;
    if(verify>=0)sceIoClose(verify);
   }
  }
 }
 if(!ok || epoch!=prefetch_generation) {
  sceIoRemove(PREFETCH_PATH);
  prefetch_state=0;
 }else prefetch_state=2;
 sceKernelExitDeleteThread(0);
 return 0;
}
int iptvita_network_prefetch(const char *url)
{
 if(!url || (strncmp(url,"http://",7)!=0 &&
              strncmp(url,"https://",8)!=0))return -1;
 size_t n=strlen(url);
 if(n<8 || n>=sizeof(prefetch_url) || prefetch_state==1)return -1;
 if(prefetch_state==2 && strcmp(prefetch_url,url)==0)return 0;
 prefetch_state=0;
 sceIoRemove(PREFETCH_PATH);
 memcpy(prefetch_url,url,n+1);
 ++prefetch_generation;
 prefetch_state=1;
 SceUID thread=sceKernelCreateThread("iptvita_prefetch",prefetch_worker,
                                    0x10000100,128*1024,0,0,NULL);
 if(thread<0){prefetch_state=0;return -1;}
 if(sceKernelStartThread(thread,0,NULL)<0) {
  sceKernelDeleteThread(thread);
  prefetch_state=0;return -1;
 }
 return 0;
}
int iptvita_network_prefetch_ready(const char *url)
{
 return url && prefetch_state==2 && strcmp(prefetch_url,url)==0;
}
void iptvita_network_prefetch_clear(void)
{
 ++prefetch_generation;
 if(prefetch_state!=1) {
  prefetch_state=0;
  prefetch_url[0]=0;
  sceIoRemove(PREFETCH_PATH);
 }
}
unsigned iptvita_network_prefetch_hits(void){return prefetch_hits;}
