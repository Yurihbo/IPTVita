#include "video_preview.h"
#include "avc_decoder.h"
#include "h264_sps.h"
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "h264_au.h"
#include <stdlib.h>
#include <string.h>

static IptvitaAvcDecoder dec;
static int open;
static unsigned char *rgba;
static size_t rgba_size;
static unsigned char *ready_rgba;
static size_t ready_size;
static int ready_w,ready_h,ready_pending;
static SceUID queue_mutex=-1, decode_thread=-1;
static volatile int thread_running,thread_paused;
static unsigned long long decode_total_us;
static unsigned decode_samples;
static vita2d_texture *texture;
static IptvitaPreviewStats st;
static int wait_for_idr=1, recovery_attempts=0, reference_frames=4;
static int decoder_fatal;
static uint64_t newest_pts;
static int have_newest_pts;
#define FRAME_QUEUE 180
#define QUEUE_BYTES (16u*1024u*1024u)
typedef struct {unsigned char *data;size_t size;uint64_t pts;int has_pts,key;} EncodedFrame;
static EncodedFrame frames[FRAME_QUEUE];
static unsigned head,tail,count;
static size_t total_bytes;
static unsigned long long due_at;
static uint64_t previous_pts;
static int previous_pts_valid;
static unsigned long long clock_us(void){return (unsigned long long)sceKernelGetProcessTimeWide();}


static void lock_queue(void){if(queue_mutex>=0)sceKernelLockMutex(queue_mutex,1,NULL);}
static void unlock_queue(void){if(queue_mutex>=0)sceKernelUnlockMutex(queue_mutex,1);}
static int decode_worker(SceSize argc,void *argv);
void iptvita_preview_reset(void)
{
 thread_running=0;
 if(decode_thread>=0){
  sceKernelWaitThreadEnd(decode_thread,NULL,NULL);
  decode_thread=-1;
 }
 while(count){free(frames[head].data);head=(head+1)%FRAME_QUEUE;count--;}
 head=tail=0;total_bytes=0;due_at=0;previous_pts_valid=0;previous_pts=0;
 newest_pts=0;have_newest_pts=0;
 if(open)iptvita_avc_close(&dec);
 open=0;
 if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);texture=NULL;}
 free(rgba);rgba=NULL;rgba_size=0;
 free(ready_rgba);ready_rgba=NULL;ready_size=0;ready_pending=0;ready_w=ready_h=0;
 if(queue_mutex>=0){sceKernelDeleteMutex(queue_mutex);queue_mutex=-1;}
 memset(&st,0,sizeof st);
 wait_for_idr=1;recovery_attempts=0;reference_frames=4;decoder_fatal=0;
 decode_total_us=0;decode_samples=0;thread_paused=0;st.waiting_for_idr=1;
 queue_mutex=sceKernelCreateMutex("iptvita_video_queue",0,0,NULL);
 if(queue_mutex<0){st.decode_error=queue_mutex;st.fatal=1;return;}
 thread_running=1;
 decode_thread=sceKernelCreateThread("iptvita_h264_worker",decode_worker,
                                    0x10000100,192*1024,0,0,NULL);
 if(decode_thread<0){st.decode_error=decode_thread;st.fatal=1;thread_running=0;return;}
 int rc=sceKernelStartThread(decode_thread,0,NULL);
 if(rc<0){st.decode_error=rc;st.fatal=1;thread_running=0;
           sceKernelDeleteThread(decode_thread);decode_thread=-1;}
}
void iptvita_preview_pause(int paused){thread_paused=paused?1:0;}
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
   /* Display texture is owned by the main GPU thread. */
  }
  if(!rgba) {
   rgba=(unsigned char*)malloc(needed);
   if(rgba)rgba_size=needed;
  }
  if(!rgba) {
   st.decode_error=-31;
   iptvita_avc_close(&dec);
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
 st.last_output_us=clock_us();
 st.decoder_phase=3;
 /* Publish only completed RGBA pictures; no vita2d/GXM calls in worker. */
 size_t bytes=(size_t)((st.width+15)&~15)*((st.height+15)&~15)*4u;
 lock_queue();
 if(bytes>ready_size) {
  unsigned char *next=(unsigned char*)realloc(ready_rgba,bytes);
  if(next){ready_rgba=next;ready_size=bytes;}
 }
 if(ready_rgba && ready_size>=bytes){
  memcpy(ready_rgba,rgba,bytes);
  ready_w=st.width;ready_h=st.height;ready_pending=1;
 } else st.decode_error=-33;
 unlock_queue();
}
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key)
{
 if(!data || !size || size>IPTVITA_H264_AU_MAX || decoder_fatal || !thread_running)return;
 lock_queue();
 /* Keep a one-second B-frame reorder tolerance. Bigger regressions indicate
  * replayed segments or a discontinuity; recover at an IDR. */
 if(has_pts) {
  if(have_newest_pts && pts<newest_pts) {
   uint64_t behind=newest_pts-pts;
   if(behind>90000ULL*15 && key){newest_pts=pts;previous_pts_valid=0;}
   else if(behind>90000ULL){st.dropped++;unlock_queue();return;}
  }
  if(!have_newest_pts || pts>newest_pts)newest_pts=pts;
  have_newest_pts=1;
 }
 if(key)st.keyframes++;
 if(count==FRAME_QUEUE || size>QUEUE_BYTES-total_bytes){st.dropped++;unlock_queue();return;}
 unsigned char *copy=(unsigned char*)malloc(size);
 if(!copy){st.dropped++;unlock_queue();return;}
 memcpy(copy,data,size);
 frames[tail]=(EncodedFrame){copy,size,pts,has_pts,key};
 tail=(tail+1)%FRAME_QUEUE;count++;total_bytes+=size;
 st.queued=count;
 st.buffer_percent=count>=30?100:(int)(count*100u/30u);
 unlock_queue();
}
static int decode_worker(SceSize argc,void *argv)
{
 (void)argc;(void)argv;
 while(thread_running) {
  if(thread_paused){sceKernelDelayThread(5000);continue;}
  unsigned long long now=clock_us();
  if(due_at && now<due_at){
   unsigned long long wait=due_at-now;
   sceKernelDelayThread((unsigned)(wait>5000?5000:wait));
   continue;
  }
  EncodedFrame item={0};
  lock_queue();
  if(count){
   item=frames[head];head=(head+1)%FRAME_QUEUE;count--;
   total_bytes-=item.size;
   st.queued=count;
   st.buffer_percent=count>=30?100:(int)(count*100u/30u);
  }
  unlock_queue();
  if(!item.data){sceKernelDelayThread(2500);continue;}
  unsigned long long started=clock_us();
  decode_frame(item.data,item.size,item.pts,item.has_pts,item.key);
  unsigned long long elapsed=clock_us()-started;
  decode_total_us+=elapsed;decode_samples++;
  st.decode_avg_us=(unsigned)(decode_total_us/decode_samples);
  free(item.data);
  unsigned long long duration=33333ULL;
  if(previous_pts_valid && item.has_pts && item.pts>previous_pts){
   uint64_t delta=item.pts-previous_pts;
   if(delta>=900 && delta<=9000)duration=delta*1000000ULL/90000ULL;
  }
  previous_pts=item.pts;previous_pts_valid=item.has_pts;
  /* Deadline is absolute, so decoding time does not accumulate as drift. */
  due_at=(due_at && started<due_at+250000ULL)?due_at+duration:started+duration;
 }
 sceKernelExitDeleteThread(0);
 return 0;
}
void iptvita_preview_tick(void)
{
 lock_queue();
 if(ready_pending && ready_rgba && ready_w>0 && ready_h>0){
  if(!texture || vita2d_texture_get_width(texture)!=(unsigned)ready_w ||
                  vita2d_texture_get_height(texture)!=(unsigned)ready_h){
   if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);}
   texture=vita2d_create_empty_texture((unsigned)ready_w,(unsigned)ready_h);
  }
  if(texture){
   unsigned char *dst=(unsigned char*)vita2d_texture_get_datap(texture);
   unsigned dst_stride=vita2d_texture_get_stride(texture);
   unsigned src_stride=(unsigned)((ready_w+15)&~15)*4u;
   if(dst && dst_stride>=(unsigned)ready_w*4u){
    for(int y=0;y<ready_h;y++)
     memcpy(dst+(size_t)y*dst_stride,ready_rgba+(size_t)y*src_stride,(size_t)ready_w*4u);
    st.presented++;
   }
  }
  ready_pending=0;
 }
 unlock_queue();
}
IptvitaPreviewStats iptvita_preview_stats(void)
{
 IptvitaPreviewStats snapshot;
 lock_queue();snapshot=st;unlock_queue();
 return snapshot;
}
void iptvita_preview_draw(int x,int y,int width,int height,int fill)
{
 if(!texture || !st.decoded || !st.width || !st.height || width<=0 || height<=0)return;
 float sx=(float)width/(float)st.width,sy=(float)height/(float)st.height;
 if(fill) {
  /* Preserve the image aspect ratio, center-cropping the excess rather than
   * stretching a 4:3 source into the Vita's 16:9 screen. */
  float scale=sx>sy?sx:sy;
  float crop_w=(float)width/scale,crop_h=(float)height/scale;
  float crop_x=((float)st.width-crop_w)*0.5f;
  float crop_y=((float)st.height-crop_h)*0.5f;
  vita2d_draw_texture_part_scale(texture,(float)x,(float)y,
    crop_x,crop_y,crop_w,crop_h,scale,scale);
 }else{
  float scale=sx<sy?sx:sy;
  float dw=(float)st.width*scale,dh=(float)st.height*scale;
  vita2d_draw_texture_scale(texture,x+((float)width-dw)*0.5f,
    y+((float)height-dh)*0.5f,scale,scale);
 }
}
