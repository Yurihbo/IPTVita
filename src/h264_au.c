#include "h264_au.h"
#include <stdlib.h>
#include <string.h>

/* Reconstruct Annex-B access units from PES elementary-stream payloads.
 * Boundaries: AUD or first macroblock of a new VCL picture. No decoding here.
 * Works for usual AVC-in-TS with complete start codes inside each PES buffer.
 * A subsequent network stage must handle split headers and discontinuities. */
static size_t prefix(const unsigned char *d,size_t n,size_t i)
{
 if(i+3<n && d[i]==0 && d[i+1]==0 && d[i+2]==0 && d[i+3]==1)return 4;
 if(i+2<n && d[i]==0 && d[i+1]==0 && d[i+2]==1)return 3;
 return 0;
}
static size_t find_nal(const unsigned char *d,size_t n,size_t from)
{
 for(size_t i=from;i+3<=n;i++)if(prefix(d,n,i))return i;
 return n;
}
/* H.264 unsigned Exp-Golomb: first_mb_in_slice. Returns -1 if invalid. */
static int first_mb(const unsigned char *nal,size_t n)
{
 if(n<2)return -1;
 unsigned z=0,bitpos=8;
 for(;;) {
  if(bitpos>=n*8)return -1;
  int bit=(nal[bitpos/8]>>(7-(bitpos%8)))&1;
  bitpos++;
  if(bit)break;
  if(++z>23)return -1;
 }
 unsigned value=1;
 for(unsigned i=0;i<z;i++) {
  if(bitpos>=n*8)return -1;
  value=(value<<1)|((nal[bitpos/8]>>(7-(bitpos%8)))&1);
  bitpos++;
 }
 return (int)(value-1);
}
static int has_nal(const unsigned char *buf,size_t size,int nal_type)
{
 for(size_t i=0;i+4<size;i++)
  if(buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 &&
     buf[i+3]==1 && (buf[i+4]&31)==nal_type)return 1;
 return 0;
}
static void prepend(IptvitaH264Au *a,const unsigned char *data,size_t size)
{
 if(!size || !a->buffer || size>IPTVITA_H264_AU_MAX-a->used)return;
 memmove(a->buffer+size,a->buffer,a->used);
 memcpy(a->buffer,data,size);
 a->used+=size;
}
static void emit(IptvitaH264Au *a)
{
 if(a->has_vcl && a->used && a->sink) {
  /* A live segment can start on an IDR without in-band SPS/PPS.
   * Repeat the latest authorized codec metadata before that keyframe. */
  if(a->has_idr) {
   if(!has_nal(a->buffer,a->used,8))prepend(a,a->pps,a->pps_size);
   if(!has_nal(a->buffer,a->used,7))prepend(a,a->sps,a->sps_size);
  }
  a->sink(a->user,a->buffer,a->used,a->pts,a->has_pts,a->has_idr);
  a->frames++;
 }
 a->used=0;a->has_vcl=0;a->has_idr=0;a->has_pts=0;
}
/* A NAL may be split across multiple PES payloads. Keep incomplete trailing
 * bytes until the next Annex-B start code arrives, instead of submitting a
 * truncated slice that the Vita AVC decoder rejects as invalid parameters. */
static void consume_nal(IptvitaH264Au *a,const unsigned char *data,size_t n)
{
 size_t sc=prefix(data,n,0);
 if(!sc || n<=sc)return;
 size_t nalpos=sc;
 int type=data[nalpos]&31;
 int vcl=type>=1 && type<=5;
 int boundary=(type==9 && a->has_vcl) ||
     (vcl && a->has_vcl && first_mb(data+nalpos,n-nalpos)==0);
 if(boundary)emit(a);
 size_t payload=n-nalpos;
 if(payload+4>IPTVITA_H264_AU_MAX-a->used) {
  a->dropped++;
  a->used=0;a->has_vcl=0;a->has_idr=0;a->has_pts=0;
  return;
 }
 static const unsigned char sc4[4]={0,0,0,1};
 memcpy(a->buffer+a->used,sc4,4);a->used+=4;
 memcpy(a->buffer+a->used,data+nalpos,payload);a->used+=payload;
 if(vcl)a->has_vcl=1;
 if(type==5)a->has_idr=1;
 if(type==7 && payload+4<=sizeof(a->sps)) {
  memcpy(a->sps,sc4,4);memcpy(a->sps+4,data+nalpos,payload);
  a->sps_size=payload+4;
 }
 if(type==8 && payload+4<=sizeof(a->pps)) {
  memcpy(a->pps,sc4,4);memcpy(a->pps+4,data+nalpos,payload);
  a->pps_size=payload+4;
 }
 a->units++;
}
static void consume_pending(IptvitaH264Au *a,int finish)
{
 while(a->pending_used) {
  size_t at=find_nal(a->pending,a->pending_used,0);
  if(at>=a->pending_used) {
   /* A start code can span PES boundaries; preserve the final three bytes. */
   if(!finish && a->pending_used>3) {
    memmove(a->pending,a->pending+a->pending_used-3,3);
    a->pending_used=3;
   } else if(finish)a->pending_used=0;
   return;
  }
  if(at) {
   memmove(a->pending,a->pending+at,a->pending_used-at);
   a->pending_used-=at;
  }
  size_t start=prefix(a->pending,a->pending_used,0);
  if(!start)return;
  size_t next=find_nal(a->pending,a->pending_used,start);
  if(next>=a->pending_used && !finish)return;
  size_t len=next<a->pending_used?next:a->pending_used;
  if(len>start) {
   if(a->used==0 && a->pending_has_pts) {
    a->pts=a->pending_pts;a->has_pts=1;
   }
   consume_nal(a,a->pending,len);
  }
  if(len>=a->pending_used){a->pending_used=0;return;}
  memmove(a->pending,a->pending+len,a->pending_used-len);
  a->pending_used-=len;
 }
}
int iptvita_h264_au_open(IptvitaH264Au *a,IptvitaAuSink sink,void *user)
{
 if(!a || !sink)return -1;
 memset(a,0,sizeof(*a));
 a->buffer=malloc(IPTVITA_H264_AU_MAX);
 a->pending_capacity=IPTVITA_H264_AU_MAX;
 a->pending=malloc(a->pending_capacity);
 if(!a->buffer || !a->pending) {
  free(a->buffer);free(a->pending);
  a->buffer=a->pending=NULL;return -2;
 }
 a->sink=sink;a->user=user;return 0;
}
void iptvita_h264_au_pes(void *user,const unsigned char *data,size_t size,uint64_t pts,int has_pts)
{
 IptvitaH264Au *a=(IptvitaH264Au*)user;
 if(!a || !a->buffer || !a->pending || !data || !size)return;
 if(!a->pending_used){a->pending_pts=pts;a->pending_has_pts=has_pts;}
 if(size>a->pending_capacity-a->pending_used) {
  consume_pending(a,0);
  if(size>a->pending_capacity-a->pending_used) {
   a->dropped++;a->pending_used=0;return;
  }
 }
 memcpy(a->pending+a->pending_used,data,size);
 a->pending_used+=size;
 consume_pending(a,0);
}
void iptvita_h264_au_flush(IptvitaH264Au *a)
{
 if(!a)return;
 if(a->pending)consume_pending(a,1);
 emit(a);
}
void iptvita_h264_au_close(IptvitaH264Au *a)
{
 if(!a)return;
 free(a->buffer);free(a->pending);
 a->buffer=a->pending=NULL;a->used=a->pending_used=0;a->has_vcl=0;
}
