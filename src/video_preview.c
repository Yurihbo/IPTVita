#include "video_preview.h"
#include "avc_decoder.h"
#include "h264_sps.h"
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "h264_au.h"
#include "aac_audio.h"
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
static uint64_t newest_pts;
static int have_newest_pts;
#define FRAME_QUEUE 180
#define QUEUE_BYTES (16u*1024u*1024u)
/* Short compressed-frame cache absorbs download gaps without requiring
 * a large decoded frame pool on the Vita. Re-prime after an underrun. */
#define START_FRAMES 18
#define REBUFFER_FRAMES 12
typedef struct {unsigned char *data;size_t size;uint64_t pts;int has_pts,key;} EncodedFrame;
static EncodedFrame frames[FRAME_QUEUE];
static unsigned head,tail,count;
static SceUID queue_mutex=-1;
static void queue_lock(void){if(queue_mutex>=0)sceKernelLockMutex(queue_mutex,1,NULL);}
static void queue_unlock(void){if(queue_mutex>=0)sceKernelUnlockMutex(queue_mutex,1);}
static size_t total_bytes;
static unsigned long long due_at;
static int playback_primed;
static uint64_t previous_pts;
static int previous_pts_valid;
/* Video-clock fallback uses stream PTS rather than cumulative frame delays. */
static uint64_t video_base_pts;
static unsigned long long video_base_us;
static int video_clock_valid;
static int audio_master_active;
static unsigned long long clock_us(void){return (unsigned long long)sceKernelGetProcessTimeWide();}


void iptvita_preview_reset(void)
{
 if(queue_mutex<0)queue_mutex=sceKernelCreateMutex("iptvita_video_queue",0,0,NULL);
 queue_lock();
 while(count) {
  free(frames[head].data);
  head=(head+1)%FRAME_QUEUE;count--;
 }
 head=tail=0;total_bytes=0;due_at=0;playback_primed=0;previous_pts_valid=0;previous_pts=0;
 video_clock_valid=audio_master_active=0;video_base_pts=video_base_us=0;
 queue_unlock();
 newest_pts=0;have_newest_pts=0;
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
   queue_lock();
   while(count) {
    free(frames[head].data);
    head=(head+1)%FRAME_QUEUE;
    count--;
   }
   total_bytes=0;tail=head;st.queued=0;
   queue_unlock();
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
 queue_lock();
 /* Video decoder accepts B-frames out of PTS order (usually a few hundred
  * milliseconds), but a whole fragment replaying seconds into the past
  * must not be presented as live content. Reset only at a keyframe across
  * an actual large timestamp discontinuity. */
 if(has_pts) {
  if(have_newest_pts && pts<newest_pts) {
   uint64_t behind=newest_pts-pts;
   if(behind>90000ULL*15 && key) {
    newest_pts=pts;
    previous_pts_valid=0;
   }else if(behind>90000ULL) {
    st.dropped++;
    queue_unlock();
    return;
   }
  }
  if(!have_newest_pts || pts>newest_pts)newest_pts=pts;
  have_newest_pts=1;
 }
 if(key)st.keyframes++;
 if(count==FRAME_QUEUE || size>QUEUE_BYTES-total_bytes){st.dropped++;queue_unlock();return;}
 unsigned char *copy=(unsigned char*)malloc(size);
 if(!copy){queue_unlock();return;}
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
 st.buffer_percent=count>=30?100:(int)(count*100u/30u);
 queue_unlock();
}
void iptvita_preview_tick(void)
{
 queue_lock();
 if(!count) {
  /* Exhausted buffer: don't decode the next isolated frame immediately. */
  playback_primed=0;
  due_at=0;
  previous_pts_valid=0;
  video_clock_valid=audio_master_active=0;
  queue_unlock();
  return;
 }
 if(!playback_primed) {
  if(count<(st.decoded?REBUFFER_FRAMES:START_FRAMES) &&
     total_bytes<QUEUE_BYTES/2) {queue_unlock();return;}
  playback_primed=1;
  due_at=0;
  previous_pts_valid=0;
  video_clock_valid=audio_master_active=0;
 }
 unsigned long long now=clock_us();
 /* Audio is the preferred reference only while its playhead is fresh.
  * MPEG timestamps wrap every 2^33 ticks; compare the shortest signed
  * difference so the rollover cannot look like a multi-hour rewind. */
 if(frames[head].has_pts) {
  uint64_t audio_pts=0;
  int have_audio=iptvita_audio_clock(&audio_pts);
  int64_t delta=0;
  if(have_audio) {
   uint64_t diff=(frames[head].pts-audio_pts)&((1ULL<<33)-1);
   delta=diff>=(1ULL<<32)?(int64_t)diff-(1LL<<33):(int64_t)diff;
  }
  if(have_audio && delta > -90000LL*8 && delta < 90000LL*8) {
   /* Do not drive the video ahead of audio. Keep a small tolerance for
    * the renderer/PCM device's own buffering latency. */
   audio_master_active=1;
   if(delta>4500LL) {
    st.av_sync=1;
    queue_unlock();
    return;
   }
   st.av_sync=1;
  } else {
   /* No matching audio timestamps (including separate renditions):
    * fall back to a PTS-paced video clock instead of freezing. */
   if(audio_master_active)video_clock_valid=0;
   audio_master_active=0;
   st.av_sync=0;
   if(!video_clock_valid) {
    video_base_pts=frames[head].pts;
    video_base_us=now;
    video_clock_valid=1;
   }
   uint64_t diff=(frames[head].pts-video_base_pts)&((1ULL<<33)-1);
   int64_t ticks=diff>=(1ULL<<32)?(int64_t)diff-(1LL<<33):(int64_t)diff;
   if(ticks < -90000LL*3 || ticks > 90000LL*30) {
    video_base_pts=frames[head].pts;
    video_base_us=now;
   }else if(ticks>0) {
    unsigned long long due=video_base_us+(unsigned long long)ticks*100ULL/9ULL;
    if(due>now && due-now<1500000ULL){queue_unlock();return;}
    if(now>due+750000ULL) {
     video_base_pts=frames[head].pts;
     video_base_us=now;
    }
   }
  }
 }else st.av_sync=0;
 if(!frames[head].has_pts && due_at && now<due_at){queue_unlock();return;}
 EncodedFrame item=frames[head];
 head=(head+1)%FRAME_QUEUE;count--;
 total_bytes-=item.size;
 st.queued=count;
 st.buffer_percent=count>=30?100:(int)(count*100u/30u);
 queue_unlock();
 decode_frame(item.data,item.size,item.pts,item.has_pts,item.key);
 free(item.data);
 unsigned long long duration=33333ULL;
 if(previous_pts_valid && item.has_pts && item.pts>previous_pts) {
  uint64_t delta=item.pts-previous_pts;
  if(delta>=900 && delta<=9000)duration=delta*1000000ULL/90000ULL;
 }
 previous_pts=item.pts;
 previous_pts_valid=item.has_pts;
 /* Keep the video clock anchored through small render-loop delays. Simply
  * setting 'now + duration' on every tick accumulates lateness and leaves
  * video behind the continuously playing audio. Large stalls re-anchor
  * instead of fast-forwarding an entire buffered fragment. */
 if(due_at && now>=due_at && now-due_at<120000ULL)
  due_at+=duration;
 else
  due_at=now+duration;
}
void iptvita_preview_resume(void)
{
 /* The next decoded frame re-anchors the local clock after a user pause. */
 queue_lock();
 due_at=0;
 video_clock_valid=audio_master_active=0;
 previous_pts_valid=0;
 queue_unlock();
}
IptvitaPreviewStats iptvita_preview_stats(void){return st;}
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
