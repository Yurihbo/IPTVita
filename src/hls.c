#include "hls.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
static int resolve(const char *base,const char *ref,char *out,size_t cap)
{
 int n;
 if(strstr(ref,"://")) {n=snprintf(out,cap,"%s",ref);return n>=0 && (size_t)n<cap?0:-1;}
 const char *scheme=strstr(base,"://");
 if(!scheme) {n=snprintf(out,cap,"%s",ref);return n>=0 && (size_t)n<cap?0:-1;}
 if(ref[0]=='/' && ref[1]=='/') {n=snprintf(out,cap,"%.*s:%s",(int)(scheme-base),base,ref);return n>=0 && (size_t)n<cap?0:-1;}
 const char *host=scheme+3;
 const char *path=strchr(host,'/');
 if(ref[0]=='/') {n=snprintf(out,cap,"%.*s%s",(int)((path?path:base+strlen(base))-base),base,ref);return n>=0 && (size_t)n<cap?0:-1;}
 const char *end=base+strcspn(base,"?#");
 const char *slash=end;
 while(slash>host && slash[-1]!='/') --slash;
 n=snprintf(out,cap,"%.*s%s",(int)(slash-base),base,ref);
 return n>=0 && (size_t)n<cap?0:-1;
}
int iptvita_hls_inspect(const char *text,const char *base,IptvitaHlsInfo *out)
{
 if(!text || !base || !out) return -1;
 memset(out,0,sizeof(*out));
 const char *p=text;
 if((unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf)p+=3;
 while(*p && isspace((unsigned char)*p))++p;
 if(strncmp(p,"#EXTM3U",7)!=0)return -2;
 out->is_hls=1;
 int expect_variant=0;
 while(*p) {
  const char *e=strchr(p,'\n');
  if(!e)e=p+strlen(p);
  const char *s=p;
  while(s<e && isspace((unsigned char)*s))++s;
  while(e>s && isspace((unsigned char)e[-1]))--e;
  size_t n=(size_t)(e-s);
  if(n>=18 && !strncmp(s,"#EXT-X-STREAM-INF:",18)) {out->is_master=1;expect_variant=1;}
  else if(n>=11 && !strncmp(s,"#EXT-X-KEY:",11))out->encrypted=1;
  else if(n && *s!='#') {
   char ref[IPTVITA_HLS_URL_MAX];
   if(n>=sizeof(ref))return -3;
   size_t k=n;
   memcpy(ref,s,k);ref[k]=0;
   if((out->is_master ? !out->first_media[0] : 1) &&
      resolve(base,ref,out->first_media,sizeof(out->first_media))<0)return -3;
   if(expect_variant){++out->variants;expect_variant=0;}
   else ++out->segments;
  }
  p=*e?e+1:e;
 }
 return 0;
}
