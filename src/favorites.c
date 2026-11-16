#include "favorites.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <string.h>

#define FAVORITES_DIR "ux0:data/IPTVita"
#define FAVORITES_FILE FAVORITES_DIR "/favorites.bin"
#define FAVORITES_TEMP FAVORITES_DIR "/favorites.tmp"
#define FAVORITES_MAGIC 0x49504656u /* IPFV */
#define FAVORITES_VERSION 1u
#define FAVORITES_LIMIT 2048u
typedef struct {uint64_t source_hash,channel_hash;} Fav;
typedef struct {uint32_t magic,version,count;} Header;
static Fav entries[FAVORITES_LIMIT];
static unsigned entry_count;
static int initialized;

/* 64-bit FNV-1a: keep signed/live URLs private in the favorites file
 * instead of writing their potentially expiring query tokens to disk.
 * Include the playlist source to isolate favorites across providers. */
static uint64_t hash_url(const char *s)
{
 uint64_t h=UINT64_C(14695981039346656037);
 if(!s)return 0;
 for(;*s;s++) {
  h^=(unsigned char)*s;
  h*=UINT64_C(1099511628211);
 }
 return h;
}
/* Prefer the broadcaster's stable M3U tvg-id; when unavailable, use
 * channel name + group so rotated CDN query tokens do not erase favorites.
 * Only anonymous channels fall back to their full URL. */
static uint64_t channel_hash(const IptvitaChannel *ch)
{
 if(!ch)return 0;
 const char *first=ch->tvg_id && ch->tvg_id[0]?ch->tvg_id:ch->name;
 const char *second=ch->tvg_id && ch->tvg_id[0]?"":ch->group;
 const char *tag=ch->tvg_id && ch->tvg_id[0]?"tvg-id:":"name:";
 if(!first || !*first ||
    (strcmp(first,"CANAL")==0 && (!second || !*second)))
  return hash_url(ch->url);
 uint64_t h=hash_url(tag);
 for(const char *p=first;*p;p++) {
  h^=(unsigned char)*p;h*=UINT64_C(1099511628211);
 }
 h^=0xffu;h*=UINT64_C(1099511628211);
 for(const char *p=second;*p;p++) {
  h^=(unsigned char)*p;h*=UINT64_C(1099511628211);
 }
 return h;
}
static int lookup(uint64_t source,uint64_t channel)
{
 for(unsigned i=0;i<entry_count;i++)
  if(entries[i].source_hash==source &&
     entries[i].channel_hash==channel)return (int)i;
 return -1;
}
void iptvita_favorites_load(void)
{
 if(initialized)return;
 initialized=1;
 entry_count=0;
 SceUID fd=sceIoOpen(FAVORITES_FILE,SCE_O_RDONLY,0);
 if(fd<0)return;
 Header header;
 int valid=sceIoRead(fd,&header,sizeof(header))==(int)sizeof(header) &&
           header.magic==FAVORITES_MAGIC &&
           header.version==FAVORITES_VERSION &&
           header.count<=FAVORITES_LIMIT;
 if(valid && header.count) {
  int bytes=(int)(header.count*sizeof(Fav));
  valid=sceIoRead(fd,entries,bytes)==bytes;
  if(valid)entry_count=header.count;
 }
 sceIoClose(fd);
 if(!valid)entry_count=0;
}
static int persist(void)
{
 sceIoMkdir(FAVORITES_DIR,0777);
 SceUID fd=sceIoOpen(FAVORITES_TEMP,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
 if(fd<0)return -1;
 Header header={FAVORITES_MAGIC,FAVORITES_VERSION,entry_count};
 int ok=sceIoWrite(fd,&header,sizeof(header))==(int)sizeof(header);
 if(ok && entry_count) {
  int bytes=(int)(entry_count*sizeof(Fav));
  ok=sceIoWrite(fd,entries,bytes)==bytes;
 }
 sceIoClose(fd);
 if(!ok) {sceIoRemove(FAVORITES_TEMP);return -1;}
 /* Match the existing sources.bin atomic-save convention. */
 sceIoRemove(FAVORITES_FILE);
 if(sceIoRename(FAVORITES_TEMP,FAVORITES_FILE)<0)return -1;
 return 0;
}
int iptvita_favorites_has(const char *source_url,const IptvitaChannel *channel)
{
 iptvita_favorites_load();
 if(!source_url || !channel || !*source_url || !channel->url ||
    !*channel->url)return 0;
 return lookup(hash_url(source_url),channel_hash(channel))>=0;
}
int iptvita_favorites_toggle(const char *source_url,const IptvitaChannel *channel)
{
 iptvita_favorites_load();
 if(!source_url || !channel || !*source_url || !channel->url ||
    !*channel->url)return -1;
 Fav key={hash_url(source_url),channel_hash(channel)};
 int found=lookup(key.source_hash,key.channel_hash);
 if(found>=0) {
  unsigned i=(unsigned)found;
  for(unsigned j=i;j+1<entry_count;j++)entries[j]=entries[j+1];
  --entry_count;
  if(persist()<0) {
   for(unsigned j=entry_count;j>i;j--)entries[j]=entries[j-1];
   entries[i]=key;
   ++entry_count;
   return -1;
  }
  return 0;
 }
 if(entry_count>=FAVORITES_LIMIT)return -2;
 entries[entry_count++]=key;
 if(persist()<0) {--entry_count;return -1;}
 return 1;
}
void iptvita_favorites_rebuild(IptvitaUi *ui)
{
 if(!ui)return;
 ui->favorite_count=0;
 if(ui->source_index<0 || ui->source_index>=(int)ui->sources.count)return;
 if(ui->playlist.status!=1)return;
 const char *source=ui->sources.items[ui->source_index].url;
 for(int i=0;i<ui->playlist.count &&
       ui->favorite_count<IPTVITA_MAX_CHANNELS;i++) {
  const IptvitaChannel *ch=&ui->playlist.channels[i];
  if(ch->url && iptvita_favorites_has(source,ch))
   ui->favorite_channel_indices[ui->favorite_count++]=i;
 }
}
