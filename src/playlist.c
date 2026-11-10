#include "playlist.h"
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static void copy(char *dst,size_t cap,const char *src,size_t len)
{
 if(len>=cap) len=cap-1;
 memcpy(dst,src,len); dst[len]=0;
}
static void trim(char *s)
{
 size_t n=strlen(s);
 while(n && (s[n-1]=='\r'||s[n-1]=='\n'||isspace((unsigned char)s[n-1]))) s[--n]=0;
 size_t i=0;
 while(s[i] && isspace((unsigned char)s[i])) ++i;
 if(i) memmove(s,s+i,strlen(s+i)+1);
}
static void parse_meta(const char *line,char *name,char *group)
{
 const char *comma=strrchr(line,',');
 if(comma && comma[1]) {copy(name,IPTVITA_CHANNEL_NAME,comma+1,strlen(comma+1));trim(name);}
 const char *p=strstr(line,"group-title=\"");
 if(p) {
  p+=13;
  const char *end=strchr(p,'"');
  if(end) copy(group,64,p,(size_t)(end-p));
 }
}
int iptvita_playlist_load(IptvitaPlaylist *list,const char *path)
{
 memset(list,0,sizeof(*list));
 if(!path || strncmp(path,"ux0:",4)!=0) {list->status=-4;return -4;}
 SceUID fd=sceIoOpen(path,SCE_O_RDONLY,0);
 if(fd<0) {list->status=-1;return -1;}
 char line[IPTVITA_CHANNEL_URL];size_t used=0;
 char pending_name[IPTVITA_CHANNEL_NAME]="";
 char pending_group[64]="";
 int overflow=0, saw_header=0;
 unsigned char buf[2048];int n;
 while((n=sceIoRead(fd,buf,sizeof(buf)))>0) {
  for(int i=0;i<n;i++) {
   char c=(char)buf[i];
   if(c!='\n') {
    if(used+1<sizeof(line)) line[used++]=c;
    else overflow=1;
    continue;
   }
   line[used]=0;trim(line);
   if(overflow) {list->status=-8;goto done;}
   if(!overflow) {
    if(strncmp(line,"#EXTM3U",7)==0) saw_header=1;
    else if(strncmp(line,"#EXTINF:",8)==0) {
     pending_name[0]=0;pending_group[0]=0;
     parse_meta(line,pending_name,pending_group);
    } else if(line[0] && line[0]!='#' && saw_header) {
     if(list->count<IPTVITA_MAX_CHANNELS) {
      IptvitaChannel *ch=&list->channels[list->count++];
      copy(ch->url,sizeof(ch->url),line,strlen(line));
      copy(ch->name,sizeof(ch->name),pending_name[0]?pending_name:line,
           strlen(pending_name[0]?pending_name:line));
      copy(ch->group,sizeof(ch->group),pending_group,strlen(pending_group));
     } else {list->status=-3;goto done;}
     pending_name[0]=0;pending_group[0]=0;
    }
   }
   used=0;overflow=0;
  }
 }
 if(n<0) {list->status=-1;goto done;}
 if(overflow) {list->status=-8;goto done;}
 if(used && !overflow) {
  line[used]=0;trim(line);
  if(line[0] && line[0]!='#' && saw_header && list->count<IPTVITA_MAX_CHANNELS) {
   IptvitaChannel *ch=&list->channels[list->count++];
   copy(ch->url,sizeof(ch->url),line,strlen(line));
   copy(ch->name,sizeof(ch->name),pending_name[0]?pending_name:line,
        strlen(pending_name[0]?pending_name:line));
   copy(ch->group,sizeof(ch->group),pending_group,strlen(pending_group));
  }
 }
 list->status=list->count?1:-2;
done:
 sceIoClose(fd);
 return list->status;
}
