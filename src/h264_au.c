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
static void emit(IptvitaH264Au *a)
{
 if(a->has_vcl && a->used && a->sink) {
  a->sink(a->user,a->buffer,a->used,a->pts,a->has_pts,a->has_idr);
  a->frames++;
 }
 a->used=0;a->has_vcl=0;a->has_idr=0;a->has_pts=0;
}
int iptvita_h264_au_open(IptvitaH264Au *a,IptvitaAuSink sink,void *user)
{
 if(!a || !sink)return -1;
 memset(a,0,sizeof(*a));
 a->buffer=malloc(IPTVITA_H264_AU_MAX);
 if(!a->buffer)return -2;
 a->sink=sink;a->user=user;return 0;
}
void iptvita_h264_au_pes(void *user,const unsigned char *data,size_t size,uint64_t pts,int has_pts)
{
 IptvitaH264Au *a=(IptvitaH264Au*)user;
 if(!a || !a->buffer || !data)return;
 size_t at=find_nal(data,size,0);
 while(at<size) {
  size_t start_len=prefix(data,size,at);
  size_t end=find_nal(data,size,at+start_len);
  size_t nalpos=at+start_len;
  if(nalpos>=end){at=end;continue;}
  int type=data[nalpos]&31;
  int vcl=type>=1 && type<=5;
  int boundary=(type==9 && a->has_vcl) ||
   (vcl && a->has_vcl && first_mb(data+nalpos,end-nalpos)==0);
  if(boundary)emit(a);
  if(a->used==0 && has_pts) {a->pts=pts;a->has_pts=1;}
  if(end-at>IPTVITA_H264_AU_MAX-a->used) {
   a->dropped++;a->used=0;a->has_vcl=0;a->has_idr=0;a->has_pts=0;
   at=end;continue;
  }
  /* Normalize all start codes to 00 00 00 01 for the hardware decoder. */
  size_t payload=end-nalpos;
  if(payload+4>IPTVITA_H264_AU_MAX-a->used) {
   a->dropped++;a->used=0;a->has_vcl=0;a->has_idr=0;a->has_pts=0;
   at=end;continue;
  }
  static const unsigned char sc[4]={0,0,0,1};
  memcpy(a->buffer+a->used,sc,4);a->used+=4;
  memcpy(a->buffer+a->used,data+nalpos,payload);a->used+=payload;
  if(vcl)a->has_vcl=1;
  if(type==5)a->has_idr=1;
  a->units++;
  at=end;
 }
}
void iptvita_h264_au_flush(IptvitaH264Au *a){if(a)emit(a);}
void iptvita_h264_au_close(IptvitaH264Au *a)
{
 if(!a)return;
 free(a->buffer);a->buffer=NULL;a->used=0;a->has_vcl=0;
}
