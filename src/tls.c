#include "tls.h"
#include <psp2/net/net.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/x509_crt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { int socket; } VitaSocket;
static int send_cb(void *ctx,const unsigned char *buf,size_t len)
{
 VitaSocket *s=(VitaSocket*)ctx;
 int n=send(s->socket,buf,len,0);
 return n<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:n;
}
static int recv_cb(void *ctx,unsigned char *buf,size_t len)
{
 VitaSocket *s=(VitaSocket*)ctx;
 int n=recv(s->socket,buf,len,0);
 return n<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:n;
}
static int dial(const char *host)
{
 struct addrinfo hints,*res=NULL,*it;
 memset(&hints,0,sizeof(hints));
 hints.ai_family=AF_INET;
 hints.ai_socktype=SOCK_STREAM;
 if(getaddrinfo(host,"443",&hints,&res)!=0)return -1;
 int sock=-1;
 for(it=res;it;it=it->ai_next) {
  sock=socket(it->ai_family,it->ai_socktype,it->ai_protocol);
  if(sock<0)continue;
  if(connect(sock,it->ai_addr,it->ai_addrlen)==0)break;
  close(sock);sock=-1;
 }
 freeaddrinfo(res);
 return sock;
}

static int tls_get_once(const char *url,const unsigned char *ca,size_t ca_len,
                        unsigned char *out,size_t capacity,int *http_code,
                        char *redirect,size_t redirect_cap)
{
 if(http_code)*http_code=0;
 if(redirect && redirect_cap)redirect[0]=0;
 if(!url || strncmp(url,"https://",8)!=0 || !ca || !ca_len || !out || capacity<64)return -1;
 const char *host=url+8,*slash=strchr(host,'/');
 size_t hostlen=slash?(size_t)(slash-host):strlen(host);
 if(!hostlen || hostlen>253 || memchr(host,':',hostlen))return -2;
 char domain[256];memcpy(domain,host,hostlen);domain[hostlen]=0;
 const char *path=slash?slash:"/";
 int fd=-1,result=-3;
 mbedtls_ssl_context ssl;mbedtls_ssl_config conf;
 mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
 mbedtls_x509_crt cert;
 mbedtls_ssl_init(&ssl);mbedtls_ssl_config_init(&conf);
 mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);mbedtls_x509_crt_init(&cert);
 const unsigned char seed[]="IPTVita HTTPS";
 if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,seed,sizeof(seed)-1)!=0){result=-31;goto done;}
 /* PEM certificate buffer must include its trailing NUL. */
 if(mbedtls_x509_crt_parse(&cert,ca,ca_len)!=0){result=-32;goto done;}
 if(mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)!=0){result=-33;goto done;}
 mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_REQUIRED);
 mbedtls_ssl_conf_ca_chain(&conf,&cert,NULL);
 mbedtls_ssl_conf_rng(&conf,mbedtls_ctr_drbg_random,&rng);
 if(mbedtls_ssl_setup(&ssl,&conf)!=0){result=-34;goto done;}
 if(mbedtls_ssl_set_hostname(&ssl,domain)!=0){result=-35;goto done;}
 fd=dial(domain);if(fd<0){result=-4;goto done;}
 VitaSocket sock={fd};mbedtls_ssl_set_bio(&ssl,&sock,send_cb,recv_cb,NULL);
 int rc;
 do {rc=mbedtls_ssl_handshake(&ssl);}while(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE);
 if(rc!=0){result=-5;goto done;}
 char req[16384];
 int n=snprintf(req,sizeof(req),"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: IPTVita/0.6\r\nAccept: */*\r\nConnection: close\r\n\r\n",path,domain);
 if(n<0 || n>=(int)sizeof(req)){result=-6;goto done;}
 size_t sent=0;
 while(sent<(size_t)n) {
  rc=mbedtls_ssl_write(&ssl,(const unsigned char*)req+sent,(size_t)n-sent);
  if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)continue;
  if(rc<=0){result=-7;goto done;}sent+=(size_t)rc;
 }
 size_t used=0;
 while(used<capacity) {
  rc=mbedtls_ssl_read(&ssl,out+used,capacity-used);
  if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)continue;
  if(rc==0 || rc==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)break;
  if(rc<0){result=-8;goto done;}
  used+=(size_t)rc;
 }
 if(used==capacity){result=-9;goto done;}
 out[used]=0;
 unsigned char *body=(unsigned char*)strstr((char*)out,"\r\n\r\n");
 if(!body){result=-10;goto done;}
 int code=0;sscanf((char*)out,"HTTP/%*s %d",&code);
 if(http_code)*http_code=code;
 if(code>=300 && code<400 && redirect && redirect_cap) {
  const char *line=(const char*)out;
  while((line=strstr(line,"\r\n"))!=NULL) {
   line+=2;
   if(line[0]=='\r' && line[1]=='\n')break;
   if(strncasecmp(line,"Location:",9)==0) {
    const char *value=line+9;
    while(*value==' ' || *value=='\t')value++;
    const char *end=strstr(value,"\r\n");
    if(end && end>value && (size_t)(end-value)<redirect_cap) {
     memcpy(redirect,value,(size_t)(end-value));
     redirect[end-value]=0;
    }
    break;
   }
  }
  result=-12;goto done;
 }
 if(code!=200){result=-11;goto done;}
 body+=4;
 size_t headers=(size_t)(body-out);
 size_t bytes=used-headers;
 int chunked=0;
 const char *line=(const char*)out;
 while((line=strstr(line,"\r\n"))!=NULL && line<(const char*)body-2) {
  line+=2;
  if(!strncasecmp(line,"Transfer-Encoding:",18)) {
   const char *end=strstr(line,"\r\n");
   if(end && end<(const char*)body && (strstr(line,"chunked") && strstr(line,"chunked")<end))chunked=1;
  }
 }
 if(chunked) {
  /* Decode bounded HTTP/1.1 chunks in-place; binary TS payload is allowed. */
  size_t src=0,dst=0;
  while(src<bytes) {
   size_t line_end=src;
   while(line_end+1<bytes && !(body[line_end]=='\r' && body[line_end+1]=='\n'))line_end++;
   if(line_end+1>=bytes){result=-17;goto done;}
   size_t chunk=0;
   int digits=0;
   for(size_t j=src;j<line_end && body[j]!=';';j++) {
    unsigned char c=body[j];
    int v=c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;
    if(v<0 || chunk>(capacity-(size_t)v)/16){result=-17;goto done;}
    chunk=chunk*16+(size_t)v;digits++;
   }
   if(!digits){result=-17;goto done;}
   src=line_end+2;
   if(!chunk)break;
   if(chunk>bytes-src || chunk>capacity-dst){result=-17;goto done;}
   memmove(body+dst,body+src,chunk);
   dst+=chunk;src+=chunk;
   if(src+2>bytes || body[src]!='\r' || body[src+1]!='\n'){result=-17;goto done;}
   src+=2;
  }
  bytes=dst;
 }
 memmove(out,body,bytes);out[bytes]=0;
 result=(int)bytes;
done:
 if(fd>=0)close(fd);
 mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&conf);
 mbedtls_x509_crt_free(&cert);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
 return result;
}

/* Follow up to five verified HTTPS redirects. No certificate checks disabled. */
int iptvita_tls_get(const char *url,const unsigned char *ca,size_t ca_len,
                    unsigned char *out,size_t capacity,int *http_code)
{
 if(!url || strlen(url)>=8192)return -2;
 char current[8192];
 snprintf(current,sizeof(current),"%s",url);
 for(int hop=0;hop<=5;hop++) {
  char location[8192];
  int n=tls_get_once(current,ca,ca_len,out,capacity,http_code,location,sizeof(location));
  if(n!=-12)return n;
  if(hop==5)return -13;
  if(!location[0])return -14;
  char next[8192];
  if(strncmp(location,"https://",8)==0) {
   if(strlen(location)>=sizeof(next))return -15;
   strcpy(next,location);
  }else if(strncmp(location,"http://",7)==0) {
   return -16;
  }else {
   const char *scheme=strstr(current,"://");
   if(!scheme)return -15;
   const char *host=scheme+3;
   const char *slash=strchr(host,'/');
   size_t origin=(size_t)((slash?slash:current+strlen(current))-current);
   if(location[0]=='/' && location[1]=='/') {
    if(snprintf(next,sizeof(next),"https:%s",location)>=(int)sizeof(next))return -15;
   }else if(location[0]=='/') {
    if(snprintf(next,sizeof(next),"%.*s%s",(int)origin,current,location)>=(int)sizeof(next))return -15;
   }else {
    const char *end=current+strcspn(current,"?#");
    const char *dir=end;
    while(dir>host && dir[-1]!='/')dir--;
    if(snprintf(next,sizeof(next),"%.*s%s",(int)(dir-current),current,location)>=(int)sizeof(next))return -15;
   }
  }
  memcpy(current,next,strlen(next)+1);
 }
 return -13;
}
