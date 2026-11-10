#include "network.h"
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_PATH "ux0:data/IPTVita/online.m3u"
#define MAX_BYTES (4u*1024u*1024u)
static char url_copy[4096];
static volatile int state;
static int network_ready;
static void *net_memory;
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
static int download_thread(SceSize args,void *argp)
{
 (void)args;(void)argp;
 int result=-1;
 if(init_network()<0) goto finish;
 sceIoMkdir("ux0:data/IPTVita",0777);
 SceUID fd=sceIoOpen(CACHE_PATH,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
 if(fd<0) goto finish;
 CURL *curl=curl_easy_init();
 if(!curl) {sceIoClose(fd);goto finish;}
 Download d={fd,0,0};
 curl_easy_setopt(curl,CURLOPT_URL,url_copy);
 curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
 curl_easy_setopt(curl,CURLOPT_MAXREDIRS,4L);
 curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,12L);
 curl_easy_setopt(curl,CURLOPT_TIMEOUT,50L);
 curl_easy_setopt(curl,CURLOPT_USERAGENT,"IPTVita/0.5.0");
 curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_file);
 curl_easy_setopt(curl,CURLOPT_WRITEDATA,&d);
 curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
 curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
 curl_easy_setopt(curl,CURLOPT_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
 curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTP|CURLPROTO_HTTPS);
 CURLcode code=curl_easy_perform(curl);
 long status=0;
 curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
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
 return 0;
}
int iptvita_network_start(const char *url)
{
 if(state==1 || !url)return -1;
 size_t n=strlen(url);
 if(n<8 || n>=sizeof(url_copy) ||
   (strncmp(url,"https://",8)!=0 && strncmp(url,"http://",7)!=0))return -1;
 memcpy(url_copy,url,n+1);
 state=1;
 SceUID thread=sceKernelCreateThread("iptvita_http",download_thread,0x10000100,128*1024,0,0,NULL);
 if(thread<0) {state=-1;return -1;}
 if(sceKernelStartThread(thread,0,NULL)<0) {sceKernelDeleteThread(thread);state=-1;return -1;}
 return 0;
}
int iptvita_network_state(void) {return state;}
const char *iptvita_network_cache_path(void) {return CACHE_PATH;}
void iptvita_network_reset(void) {if(state!=1)state=0;}
