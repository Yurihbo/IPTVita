#include "playlist.h"
#include <psp2/io/fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

/* The remote download and the video decoder retain their original limits:
 * only playlist entries use this compact dynamic representation. */
static void copy(char *dst,size_t cap,const char *src,size_t len)
{
 if(!cap)return;
 if(len>=cap)len=cap-1;
 memcpy(dst,src,len);
 dst[len]=0;
}
static void trim(char *s)
{
 size_t n=strlen(s);
 while(n && isspace((unsigned char)s[n-1]))s[--n]=0;
 size_t i=0;
 while(s[i] && isspace((unsigned char)s[i]))++i;
 if(i)memmove(s,s+i,strlen(s+i)+1);
}
void iptvita_playlist_clear(IptvitaPlaylist *list)
{
 if(!list)return;
 for(int i=0;i<list->count;i++)free(list->channels[i].url);
 free(list->channels);
 memset(list,0,sizeof(*list));
}
static void attribute(const char *line,const char *key,char *out,size_t cap)
{
 /* Standard M3U: group-title="News", tvg-name="Station".
  * Single-quoted attributes appear in some third-party generators. */
 const char *p=strstr(line,key);
 if(!p)return;
 p+=strlen(key);
 char quote=*p;
 if(quote!='"' && quote!='\'')return;
 const char *end=strchr(++p,quote);
 if(end)copy(out,cap,p,(size_t)(end-p));
}
static void parse_meta(const char *line,char *name,char *group)
{
 const char *comma=strrchr(line,',');
 if(comma && comma[1]) {
  copy(name,IPTVITA_CHANNEL_NAME,comma+1,strlen(comma+1));
  trim(name);
 }
 if(!name[0])attribute(line,"tvg-name=",name,IPTVITA_CHANNEL_NAME);
 attribute(line,"group-title=",group,64);
}
static int http_url(const char *url)
{
 return strncmp(url,"https://",8)==0 || strncmp(url,"http://",7)==0;
}
/* Relative HTTP URLs occur in many exported M3U files. Resolve them against
 * the submitted PLAYLIST address, never against the local cache filename. */
static int resolve_url(const char *base,const char *entry,
                       char *out,size_t cap)
{
 int n=-1;
 if(http_url(entry))n=snprintf(out,cap,"%s",entry);
 else if(!base || !http_url(base))return 0;
 else if(entry[0]=='/' && entry[1]=='/') {
  const char *colon=strchr(base,':');
  if(!colon)return 0;
  n=snprintf(out,cap,"%.*s:%s",(int)(colon-base),base,entry);
 }else {
  const char *host=strstr(base,"://");
  if(!host)return 0;
  host+=3;
  const char *slash=strchr(host,'/');
  if(entry[0]=='/') {
   size_t prefix=(size_t)((slash?slash:base+strlen(base))-base);
   n=snprintf(out,cap,"%.*s%s",(int)prefix,base,entry);
  }else {
   const char *end=base+strcspn(base,"?#");
   if(entry[0]=='?') {
    n=snprintf(out,cap,"%.*s%s",(int)(end-base),base,entry);
   }else {
    const char *dir=end;
    while(dir>host && dir[-1]!='/')--dir;
    if(!slash)dir=end;
    n=snprintf(out,cap,"%.*s%s",(int)(dir-base),base,entry);
   }
  }
 }
 return n>0 && (size_t)n<cap && http_url(out);
}
static int add_channel(IptvitaPlaylist *list,const char *name,
                       const char *group,const char *url)
{
 if(list->count>=IPTVITA_MAX_CHANNELS) {
  list->truncated=1; /* Do not discard the channels already loaded. */
  return 0;
 }
 if(list->count==list->capacity) {
  int next=list->capacity?list->capacity*2:128;
  if(next>IPTVITA_MAX_CHANNELS)next=IPTVITA_MAX_CHANNELS;
  IptvitaChannel *b=realloc(list->channels,(size_t)next*sizeof(*b));
  if(!b)return -14;
  list->channels=b;
  list->capacity=next;
 }
 size_t n=strlen(url);
 char *stored=malloc(n+1);
 if(!stored)return -14;
 memcpy(stored,url,n+1);
 IptvitaChannel *ch=&list->channels[list->count++];
 memset(ch,0,sizeof(*ch));
 ch->url=stored;
 if(name && name[0])copy(ch->name,sizeof(ch->name),name,strlen(name));
 else {
  /* No EXTM3U display name: use path tail rather than 8k of query tokens. */
  const char *part=strrchr(url,'/');
  part=part?part+1:url;
  size_t len=strcspn(part,"?#");
  if(len==0){part="CANAL";len=5;}
  copy(ch->name,sizeof(ch->name),part,len);
 }
 if(group && group[0])copy(ch->group,sizeof(ch->group),group,strlen(group));
 return 0;
}
static int process_line(IptvitaPlaylist *list,char *line,
                        const char *origin,char *name,char *group,
                        int *is_hls,int *invalid_lines)
{
 trim(line);
 if(!line[0])return 0;
 if((unsigned char)line[0]==0xef &&
    (unsigned char)line[1]==0xbb &&
    (unsigned char)line[2]==0xbf)
  memmove(line,line+3,strlen(line+3)+1); /* UTF-8 BOM */
 if(strncmp(line,"#EXT-X-",7)==0) {
  *is_hls=1;
  return 0;
 }
 if(strncmp(line,"#EXTINF:",8)==0) {
  name[0]=0;group[0]=0;
  parse_meta(line,name,group);
  return 0;
 }
 if(strncmp(line,"#EXTGRP:",8)==0) {
  copy(group,64,line+8,strlen(line+8));
  trim(group);
  return 0;
 }
 if(line[0]=='#')return 0;
 if(*is_hls)return 0; /* HLS segments/variants are not distinct channels. */
 char url[IPTVITA_CHANNEL_URL];
 if(!resolve_url(origin,line,url,sizeof(url))) {
  (*invalid_lines)++;
  name[0]=group[0]=0;
  return 0;
 }
 int rc=add_channel(list,name,group,url);
 name[0]=group[0]=0;
 return rc;
}
static int load_impl(IptvitaPlaylist *list,const char *path,const char *origin)
{
 if(!list)return -1;
 iptvita_playlist_clear(list);
 if(!path || strncmp(path,"ux0:",4)!=0) {
  list->status=-4;
  return list->status;
 }
 SceUID fd=sceIoOpen(path,SCE_O_RDONLY,0);
 if(fd<0){list->status=-1;return list->status;}
 /* Only one URL line at a time is retained. Size includes the NUL. */
 char line[IPTVITA_CHANNEL_URL];
 char pending_name[IPTVITA_CHANNEL_NAME]="";
 char pending_group[64]="";
 unsigned char chunk[2048];
 size_t used=0;
 int overflow=0,overlong=0,invalid_lines=0,is_hls=0,rc=0,n=0;
 while((n=sceIoRead(fd,chunk,sizeof(chunk)))>0) {
  for(int i=0;i<n;i++) {
   if(chunk[i]!='\n') {
    if(!overflow) {
     if(used+1<sizeof(line))line[used++]=(char)chunk[i];
     else overflow=1;
    }
    continue;
   }
   if(overflow)overlong++;
   else {
    line[used]=0;
    rc=process_line(list,line,origin,pending_name,pending_group,
                    &is_hls,&invalid_lines);
    if(rc<0)goto done;
   }
   used=0;
   overflow=0;
  }
 }
 if(n<0){rc=-1;goto done;}
 if(overflow)overlong++;
 else if(used) {
  line[used]=0;
  rc=process_line(list,line,origin,pending_name,pending_group,
                  &is_hls,&invalid_lines);
  if(rc<0)goto done;
 }
 /* A direct HLS master/media M3U8 is one channel, not an IPTV channel
  * collection. Keep its ORIGINAL public URL so the existing HLS player
  * processes variant selection and segments without any changes. */
 if(is_hls && origin && http_url(origin)) {
  iptvita_playlist_clear(list);
  rc=add_channel(list,"HLS AO VIVO","",origin);
  if(rc<0)goto done;
 }
done:
 sceIoClose(fd);
 if(rc<0) {
  iptvita_playlist_clear(list);
  list->status=rc;
 }else if(list->count)list->status=1;
 else list->status=overlong?-13:-2;
 return list->status;
}
int iptvita_playlist_load(IptvitaPlaylist *list,const char *path)
{
 return load_impl(list,path,NULL);
}
int iptvita_playlist_load_remote(IptvitaPlaylist *list,const char *path,
                                 const char *playlist_url)
{
 return load_impl(list,path,playlist_url);
}
