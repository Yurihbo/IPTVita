#include "ts_pipeline.h"
#include <string.h>

/* Independent MPEG-TS transport inspection: PAT -> PMT -> elementary stream PIDs.
 * This is a foundation for a later PES/H264/AAC decoder, not a video renderer. */
void iptvita_ts_init(IptvitaTsPipeline *p)
{
 memset(p,0,sizeof(*p));
 p->pmt_pid=-1;p->video_pid=-1;p->audio_pid=-1;
 memset(p->counters,0xff,sizeof(p->counters));
}
static int psi_section(const unsigned char *data,size_t n,const unsigned char **section,size_t *length)
{
 if(n<1)return -1;
 size_t pointer=data[0];
 if(pointer+1>=n)return -1;
 const unsigned char *s=data+pointer+1;
 size_t available=n-pointer-1;
 if(available<3)return -1;
 size_t size=3+(((size_t)s[1]&15)<<8)+s[2];
 if(size>available || size<12)return -1;
 *section=s;*length=size;return 0;
}
static void parse_pat(IptvitaTsPipeline *p,const unsigned char *s,size_t n)
{
 if(s[0]!=0 || n<12)return;
 size_t end=n-4;
 for(size_t i=8;i+4<=end;i+=4) {
  int program=(s[i]<<8)|s[i+1];
  if(program) {p->pmt_pid=((s[i+2]&31)<<8)|s[i+3];break;}
 }
}
static void parse_pmt(IptvitaTsPipeline *p,const unsigned char *s,size_t n)
{
 if(s[0]!=2 || n<16)return;
 size_t info=(((size_t)s[10]&15)<<8)|s[11];
 size_t pos=12+info,end=n-4;
 if(pos>end)return;
 /* PMT can change when a live channel inserts or leaves an advert.
  * Refresh elementary-stream PIDs instead of retaining stale ones. */
 int video_pid=-1,audio_pid=-1,video_type=0,audio_type=0;
 while(pos+5<=end) {
  int type=s[pos],pid=((s[pos+1]&31)<<8)|s[pos+2];
  size_t esinfo=(((size_t)s[pos+3]&15)<<8)|s[pos+4];
  if(type==0x1b && video_pid<0){video_pid=pid;video_type=type;}
  if((type==0x0f || type==0x11 || type==0x03 || type==0x04) &&
     (audio_pid<0 || (type==0x0f && audio_type!=0x0f))){
   audio_pid=pid;audio_type=type;
  }
  if(pos+5+esinfo>end)break;
  pos+=5+esinfo;
 }
 p->video_pid=video_pid;p->video_type=video_type;
 p->audio_pid=audio_pid;p->audio_type=audio_type;
}
static void pes_start(IptvitaTsPipeline *p,const unsigned char *data,size_t n,int video)
{
 if(n<9 || data[0]!=0 || data[1]!=0 || data[2]!=1)return;
 if(video)p->pes_video_starts++;else p->pes_audio_starts++;
 if((data[7]&0xc0)!=0x80 && (data[7]&0xc0)!=0xc0)return;
 if(data[8]<5 || n<14)return;
 const unsigned char *b=data+9;
 if((b[0]&1)!=1 || (b[2]&1)!=1 || (b[4]&1)!=1)return;
 uint64_t pts=((uint64_t)(b[0]>>1&7)<<30)|((uint64_t)b[1]<<22)|((uint64_t)(b[2]>>1)<<15)|((uint64_t)b[3]<<7)|(b[4]>>1);
 if(video){p->last_video_pts=pts;p->video_pts_valid=1;}
 else {p->last_audio_pts=pts;p->audio_pts_valid=1;}
}
static void h264_scan(IptvitaTsPipeline *p,const unsigned char *data,size_t n)
{
 /* Detect Annex-B start codes across TS packet boundaries. This is inspection
  * only; the future decoder requires complete access-unit assembly. */
 for(size_t i=0;i<n;i++) {
  unsigned char b=data[i];
  if(p->video_tail_len>=3 && p->video_tail[0]==0 && p->video_tail[1]==0 &&
     p->video_tail[2]==1) {
   unsigned type=b&31;
   if(type>0 && type<24) {
    p->h264_nal_units++;
    if(type==7)p->h264_sps++;
    if(type==8)p->h264_pps++;
    if(type==5)p->h264_idr++;
   }
  }
  if(p->video_tail_len<3)p->video_tail[p->video_tail_len++]=b;
  else {p->video_tail[0]=p->video_tail[1];p->video_tail[1]=p->video_tail[2];p->video_tail[2]=b;}
 }
}
int iptvita_ts_feed(IptvitaTsPipeline *p,const unsigned char *data,size_t size)
{
 if(!p || !data || size%188)return -1;
 for(size_t off=0;off<size;off+=188) {
  const unsigned char *ts=data+off;
  if(ts[0]!=0x47){p->sync_errors++;continue;}
  p->packets++;
  int pid=((ts[1]&31)<<8)|ts[2];
  int cc=ts[3]&15,adapt=(ts[3]>>4)&3;
  if(adapt==0)continue;
  if(adapt&1) {
   if(p->counters[pid]!=0xff && ((p->counters[pid]+1)&15)!=cc)p->continuity_errors++;
   p->counters[pid]=(unsigned char)cc;
  }
  size_t pos=4;
  if(adapt&2){if(pos>=188)continue;pos+=1+ts[pos];}
  if(!(adapt&1) || pos>=188)continue;
  if(pid==p->video_pid)p->video_packets++;
  if(pid==p->audio_pid)p->audio_packets++;
  if(pid==p->video_pid) {
   size_t skip=0;
   if((ts[1]&0x40) && 188-pos>=9 && ts[pos]==0 && ts[pos+1]==0 && ts[pos+2]==1) {
    skip=9+(size_t)ts[pos+8];
    if(skip>188-pos)skip=188-pos;
   }
   h264_scan(p,ts+pos+skip,188-pos-skip);
  }
  if(ts[1]&0x40) {
   if(pid==p->video_pid)pes_start(p,ts+pos,188-pos,1);
   if(pid==p->audio_pid)pes_start(p,ts+pos,188-pos,0);
  }
  if(!(ts[1]&0x40))continue;
  const unsigned char *section=NULL;size_t length=0;
  if(pid==0) {
   p->pat_packets++;
   if(!psi_section(ts+pos,188-pos,&section,&length))parse_pat(p,section,length);
  }else if(pid==p->pmt_pid) {
   p->pmt_packets++;
   if(!psi_section(ts+pos,188-pos,&section,&length))parse_pmt(p,section,length);
  }
 }
 return (int)p->packets;
}
