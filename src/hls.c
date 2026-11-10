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
int iptvita_hls_inspect_next(const char *text,const char *base,IptvitaHlsInfo *out,
                             unsigned long long last_sequence,int have_previous)
{
 if(!text || !base || !out) return -1;
 memset(out,0,sizeof(*out));
 const char *p=text;
 if((unsigned char)p[0]==0xef && (unsigned char)p[1]==0xbb && (unsigned char)p[2]==0xbf)p+=3;
 while(*p && isspace((unsigned char)*p))++p;
 if(strncmp(p,"#EXTM3U",7)!=0)return -2;
 out->is_hls=1;
 int expect_variant=0;
 int pending_height=0,chosen_height=0,chosen_tier=0;
 int method=0,has_iv=0;
 unsigned char current_iv[16]={0};
 char current_key[IPTVITA_HLS_URL_MAX]={0};
 unsigned long long sequence=0;
 while(*p) {
  const char *e=strchr(p,'\n');
  if(!e)e=p+strlen(p);
  const char *s=p;
  while(s<e && isspace((unsigned char)*s))++s;
  while(e>s && isspace((unsigned char)e[-1]))--e;
  size_t n=(size_t)(e-s);
  if(n>=18 && !strncmp(s,"#EXT-X-STREAM-INF:",18)) {
   out->is_master=1;expect_variant=1;
   pending_height=0;
   const char *res=strstr(s,"RESOLUTION=");
   if(res && res<e) {
    int w=0,h=0;
    if(sscanf(res+11,"%dx%d",&w,&h)==2 && w>0 && h>0)pending_height=h;
   }
  }
  else if(n>=22 && !strncmp(s,"#EXT-X-MEDIA-SEQUENCE:",22)) {
   unsigned long long value=0;
   if(sscanf(s+22,"%llu",&value)==1)sequence=value;
  }
  else if(n>=11 && !strncmp(s,"#EXT-X-KEY:",11)) {
   char line[IPTVITA_HLS_URL_MAX];
   if(n>=sizeof(line))return -3;
   memcpy(line,s,n);line[n]=0;
   const char *m=strstr(line,"METHOD=");
   method=2;
   if(m) {
    m+=7;
    if(!strncmp(m,"NONE",4) && (m[4]==',' || !m[4]))method=0;
    else if(!strncmp(m,"AES-128",7) && (m[7]==',' || !m[7]))method=1;
   }
   current_key[0]=0;has_iv=0;memset(current_iv,0,16);
   if(method==1) {
    const char *key=strstr(line,"URI=\"");
    if(key) {
     key+=5;const char *end=strchr(key,'"');
     if(!end || (size_t)(end-key)>=sizeof(line))return -3;
     char ref[IPTVITA_HLS_URL_MAX];
     size_t count=(size_t)(end-key);
     memcpy(ref,key,count);ref[count]=0;
     if(resolve(base,ref,current_key,sizeof(current_key))<0)return -3;
    }
    const char *iv=strstr(line,"IV=0x");
    if(iv) {
     iv+=5;has_iv=1;
     for(int i=0;i<16;i++) {
      unsigned value=0;
      for(int j=0;j<2;j++) {
       char c=iv[2*i+j];
       int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
       if(d<0){method=2;has_iv=0;break;}
       value=value*16+(unsigned)d;
      }
      if(method!=1)break;
      current_iv[i]=(unsigned char)value;
     }
    }
   }
  }
  else if(n && *s!='#') {
   char ref[IPTVITA_HLS_URL_MAX];
   if(n>=sizeof(ref))return -3;
   size_t k=n;
   memcpy(ref,s,k);ref[k]=0;
   if(expect_variant) {
    ++out->variants;expect_variant=0;
    /* Prefer an SD profile first. High-res hardware AVC can exhaust
     * reference-picture memory even when the nominal 720p limit fits. */
    int tier=pending_height>0 && pending_height<=480?3:
             pending_height>0 && pending_height<=720?2:1;
    int select=!out->first_media[0] ||
      tier>chosen_tier || (tier==chosen_tier &&
                          pending_height>chosen_height && pending_height<=720);
    if(select) {
     if(resolve(base,ref,out->first_media,sizeof(out->first_media))<0)return -3;
     chosen_height=pending_height;
     chosen_tier=tier;
    }
   } else {
    unsigned long long segment_number=sequence+(unsigned long long)out->segments;
    ++out->segments;
    /* On initial tune-in, use newest segment. Afterwards consume each
     * numbered segment in order so audio and video are not skipped. */
    int select=have_previous ?
      (!out->first_media[0] && segment_number>last_sequence) : 1;
    if(select) {
     if(resolve(base,ref,out->first_media,sizeof(out->first_media))<0)return -3;
     out->cipher_method=method;
     out->encrypted=method!=0;
     out->iv_explicit=has_iv;
     memcpy(out->iv,current_iv,16);
     unsigned long long iv_sequence=segment_number;
     if(method==1 && !has_iv) {
      for(int byte=15;byte>=8;byte--) {
       out->iv[byte]=(unsigned char)(iv_sequence&255);
       iv_sequence>>=8;
      }
     }
     out->media_sequence=segment_number;
     snprintf(out->key_url,sizeof(out->key_url),"%s",current_key);
    }
   }
  }
  p=*e?e+1:e;
 }
 return 0;
}

int iptvita_hls_inspect(const char *text,const char *base,IptvitaHlsInfo *out)
{
 return iptvita_hls_inspect_next(text,base,out,0,0);
}
