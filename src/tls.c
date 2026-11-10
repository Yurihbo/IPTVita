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

int iptvita_tls_get(const char *url,const unsigned char *ca,size_t ca_len,
                    unsigned char *out,size_t capacity,int *http_code)
{
 if(http_code)*http_code=0;
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
 if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,seed,sizeof(seed)-1)!=0)goto done;
 /* PEM certificate buffer must include its trailing NUL. */
 if(mbedtls_x509_crt_parse(&cert,ca,ca_len)!=0)goto done;
 if(mbedtls_ssl_config_defaults(&conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)!=0)goto done;
 mbedtls_ssl_conf_authmode(&conf,MBEDTLS_SSL_VERIFY_REQUIRED);
 mbedtls_ssl_conf_ca_chain(&conf,&cert,NULL);
 mbedtls_ssl_conf_rng(&conf,mbedtls_ctr_drbg_random,&rng);
 if(mbedtls_ssl_setup(&ssl,&conf)!=0 || mbedtls_ssl_set_hostname(&ssl,domain)!=0)goto done;
 fd=dial(domain);if(fd<0){result=-4;goto done;}
 VitaSocket sock={fd};mbedtls_ssl_set_bio(&ssl,&sock,send_cb,recv_cb,NULL);
 int rc;
 do {rc=mbedtls_ssl_handshake(&ssl);}while(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE);
 if(rc!=0){result=-5;goto done;}
 char req[2048];
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
 if(code!=200){result=-11;goto done;}
 body+=4;
 size_t headers=(size_t)(body-out);
 size_t bytes=used-headers;
 memmove(out,body,bytes);out[bytes]=0;
 result=(int)bytes;
done:
 if(fd>=0)close(fd);
 mbedtls_ssl_free(&ssl);mbedtls_ssl_config_free(&conf);
 mbedtls_x509_crt_free(&cert);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
 return result;
}
