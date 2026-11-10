#include "video_preview.h"
#include "avc_decoder.h"
#include "h264_sps.h"
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include "h264_au.h"
#include <stdlib.h>
#include <string.h>

static IptvitaAvcDecoder dec;
static int open;
static unsigned char *rgba;
static size_t rgba_size;
static vita2d_texture *texture;
static IptvitaPreviewStats st;
static int wait_for_idr=1, recovery_attempts=0, reference_frames=4;
static int decoder_fatal;
#define FRAME_QUEUE 120
#define QUEUE_BYTES (12u*1024u*1024u)
typedef struct {unsigned char *data;size_t size;uint64_t pts;int has_pts,key;} EncodedFrame;
static EncodedFrame frames[FRAME_QUEUE];
static unsigned head,tail,count;
static size_t total_bytes;
static unsigned long long due_at;
static uint64_t previous_pts;
static int previous_pts_valid;
static unsigned long long clock_us(void){return (unsigned long long)sceKernelGetProcessTimeWide();}


void iptvita_preview_reset(void)
{
 while(count) {
  free(frames[head].data);
  head=(head+1)%FRAME_QUEUE;count--;
 }
 head=tail=0;total_bytes=0;due_at=0;previous_pts_valid=0;previous_pts=0;
 if(open)iptvita_avc_close(&dec);
 open=0;
 if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);texture=NULL;}
 free(rgba);rgba=NULL;rgba_size=0;
 memset(&st,0,sizeof st);
 wait_for_idr=1;recovery_attempts=0;reference_frames=4;decoder_fatal=0;
 st.waiting_for_idr=1;
}
static void decode_frame(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key)
{
 if(!data || !size || decoder_fatal)return;
 /* Never start the hardware on an arbitrary P or B frame of a live stream.
  * Wait for a random-access (IDR) picture after tuning or after recovery. */
 if(wait_for_idr && !key){st.dropped++;return;}
 if(!open) {
  int w=0,h=0;
  if(iptvita_h264_dimensions(data,size,&w,&h)<0) {
   /* On recovery, some encoders send SPS/PPS only at playlist boundaries. */
   w=st.width;h=st.height;
   if(!w || !h)return;
  }
  if(w>1280 || h>720){st.decode_error=-30;return;}
  int rc=iptvita_avc_open(&dec,w,h,reference_frames);
  if(rc<0){
   st.decode_error=rc;
   st.width=w;st.height=h;
   st.decoder_phase=1;
   return;
  }
  st.decoder_phase=2;
  size_t stride=(size_t)((w+15)&~15);
  size_t lines=(size_t)((h+15)&~15);
  size_t needed=stride*lines*4u;
  if(rgba_size<needed || st.width!=w || st.height!=h) {
   free(rgba);rgba=NULL;rgba_size=0;
   if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);texture=NULL;}
  }
  if(!rgba) {
   rgba=(unsigned char*)malloc(needed);
   if(rgba)rgba_size=needed;
  }
  if(!texture)texture=vita2d_create_empty_texture((unsigned)w,(unsigned)h);
  if(!rgba || !texture) {
   st.decode_error=-31;
   iptvita_avc_close(&dec);
   if(texture){vita2d_free_texture(texture);texture=NULL;}
   free(rgba);rgba=NULL;rgba_size=0;
   return;
  }
  st.width=w;st.height=h;
  open=1;
 }
 st.submitted++;
 int rc=iptvita_avc_decode(&dec,data,size,pts,has_pts,rgba,rgba_size);
 if(rc<0) {
  /* A damaged live unit is not necessarily a fatal decoder failure.
   * 0x8062000D means invalid bitstream; skip and await the next IDR.
   * 0x8062000A means busy/full; don't lock the player on one AU. */
  if((unsigned)rc==0x8062000Du || (unsigned)rc==0x8062000Au) {
   st.dropped++;
   if((unsigned)rc==0x8062000Du) {
    wait_for_idr=1;
    st.waiting_for_idr=1;
   }
   return;
  }
  st.decode_error=rc;
  st.decoder_phase=2;
  unsigned error=(unsigned)rc;
  int memory_error=error==0x80620009u || error==0x80620007u ||
                   error==0x80620809u || error==0x80620807u;
  int parameter_error=error==0x80620002u || error==0x80620802u;
  if((memory_error || parameter_error) && recovery_attempts<2) {
   /* Reinitialize on the next IDR with room for more reference pictures.
    * Live streams can require more than four reference frames. */
   iptvita_avc_close(&dec);
   open=0;
   wait_for_idr=1;
   recovery_attempts++;
   st.recoveries=recovery_attempts;
   st.waiting_for_idr=1;
   /* Parameter failure after PTS fallback needs a clean decoder state,
    * while picture-memory exhaustion benefits from a larger ref pool. */
   reference_frames=memory_error?
       (recovery_attempts==1?6:8):
       (recovery_attempts==1?3:2);
  } else {
   /* Avoid infinite decode loops and unbounded compressed frame churn.
    * Keep the exact SDK error visible until the channel is reopened. */
   decoder_fatal=1;
   st.fatal=1;
   while(count) {
    free(frames[head].data);
    head=(head+1)%FRAME_QUEUE;
    count--;
   }
   total_bytes=0;tail=head;st.queued=0;
  }
  return;
 }
 wait_for_idr=0;
 st.waiting_for_idr=0;
 st.decode_error=0;
 if(rc==0)return;
 st.decoded++;
 st.decoder_phase=3;
 unsigned char *dst=(unsigned char*)vita2d_texture_get_datap(texture);
 unsigned dst_stride=vita2d_texture_get_stride(texture);
 unsigned src_stride=(unsigned)((st.width+15)&~15)*4u;
 if(!dst || dst_stride<(unsigned)st.width*4u){st.decode_error=-32;return;}
 for(int y=0;y<st.height;y++)
  memcpy(dst+(size_t)y*dst_stride,rgba+(size_t)y*src_stride,(size_t)st.width*4u);
}
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key)
{
 if(!data || !size || size>IPTVITA_H264_AU_MAX || decoder_fatal)return;
 if(key)st.keyframes++;
 if(count==FRAME_QUEUE || size>QUEUE_BYTES-total_bytes){st.dropped++;return;}
 unsigned char *copy=(unsigned char*)malloc(size);
 if(!copy)return;
 memcpy(copy,data,size);
 frames[tail].data=copy;
 frames[tail].size=size;
 frames[tail].pts=pts;
 frames[tail].has_pts=has_pts;
 frames[tail].key=key;
 tail=(tail+1)%FRAME_QUEUE;
 count++;
 total_bytes+=size;
 st.queued=count;
}
void iptvita_preview_tick(void)
{
 if(!count)return;
 unsigned long long now=clock_us();
 if(due_at && now<due_at)return;
 EncodedFrame item=frames[head];
 head=(head+1)%FRAME_QUEUE;count--;
 total_bytes-=item.size;
 st.queued=count;
 decode_frame(item.data,item.size,item.pts,item.has_pts,item.key);
 free(item.data);
 unsigned long long duration=33333ULL;
 if(previous_pts_valid && item.has_pts && item.pts>previous_pts) {
  uint64_t delta=item.pts-previous_pts;
  if(delta>=900 && delta<=9000)duration=delta*1000000ULL/90000ULL;
 }
 previous_pts=item.pts;
 previous_pts_valid=item.has_pts;
 due_at=now+duration;
}
IptvitaPreviewStats iptvita_preview_stats(void){return st;}
void iptvita_preview_draw(int x,int y,int width,int height)
{
 if(!texture || !st.decoded || !st.width || !st.height)return;
 float sx=(float)width/(float)st.width,sy=(float)height/(float)st.height;
 float scale=sx<sy?sx:sy;
 int dw=(int)(st.width*scale),dh=(int)(st.height*scale);
 vita2d_draw_texture_scale(texture,(float)(x+(width-dw)/2),(float)(y+(height-dh)/2),scale,scale);
}
