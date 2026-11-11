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
/* Small bounded decoded-picture queue. AVCDEC reference buffers are separate;
 * these are CPU RGBA staging surfaces and are allocated only on stream open.
 * Three slots allow decoding ahead while the display waits for audio PTS. */
#define DISPLAY_SLOTS 3
typedef struct {
 unsigned char *rgba;
 uint64_t pts;
 unsigned long long ready_at;
 int has_pts;
} DisplayPicture;
static DisplayPicture pictures[DISPLAY_SLOTS];
static unsigned picture_head,picture_tail,picture_count,picture_capacity;
static size_t rgba_size;
static vita2d_texture *texture;
static unsigned texture_width,texture_height;
/* Only AVC worker calls sceAvcdecDecode. The UI alone manages vita2d. */
static SceUID decoder_thread=-1;
static volatile int decoder_stop;
static volatile int decoder_paused;
static int worker_enabled;
static unsigned long long last_decoder_us;
static IptvitaPreviewStats st;
static int wait_for_idr=1, recovery_attempts=0, reference_frames=4;
static int decoder_fatal;
static uint64_t newest_pts;
static int have_newest_pts;
#define FRAME_QUEUE 240
#define QUEUE_BYTES (24u*1024u*1024u)
/* Short compressed-frame cache absorbs download gaps without requiring
 * a large decoded frame pool on the Vita. Re-prime after an underrun. */
#define START_FRAMES 24
#define REBUFFER_MIN 24
#define REBUFFER_MAX 72
#define REBUFFER_STEP 12
#define STABLE_DECAY_US 45000000ULL
typedef struct {unsigned char *data;size_t size;uint64_t pts;int has_pts,key;} EncodedFrame;
static EncodedFrame frames[FRAME_QUEUE];
static unsigned head,tail,count;
static SceUID queue_mutex=-1;
static void queue_lock(void){if(queue_mutex>=0)sceKernelLockMutex(queue_mutex,1,NULL);}
static void queue_unlock(void){if(queue_mutex>=0)sceKernelUnlockMutex(queue_mutex,1);}
static size_t total_bytes;
static unsigned long long due_at;
static int playback_primed;
static unsigned rebuffer_target=REBUFFER_MIN;
static unsigned long long last_underrun_us;
static unsigned long long last_target_decay_us;
static int saw_picture;
static uint64_t previous_pts;
static int previous_pts_valid;
/* Video-clock fallback uses stream PTS rather than cumulative frame delays. */
static uint64_t video_base_pts;
static unsigned long long video_base_us;
static int video_clock_valid;
static int audio_master_active;
/* The hardware decoder may buffer B pictures and return a picture on a later
 * access unit. Keep its output in the existing RGBA buffer until presentation
 * time; don't schedule compressed decode-order AUs by presentation PTS. */
#define PTS_WINDOW 24
static uint64_t pending_pts[PTS_WINDOW];
static unsigned pending_count;
static uint64_t last_shown_pts;
static int last_shown_valid;
static unsigned long long clock_us(void){return (unsigned long long)sceKernelGetProcessTimeWide();}
static int64_t pts_difference(uint64_t a,uint64_t b)
{
 uint64_t d=(a-b)&((1ULL<<33)-1);
 return d>=(1ULL<<32)?(int64_t)d-(1LL<<33):(int64_t)d;
}
/* Decode the first two slice-header Exp-Golomb fields: first_mb and
 * slice_type. Only this classification is needed; reference management
 * remains entirely the hardware decoder's responsibility. */
static int read_ue(const unsigned char *p,size_t bytes,unsigned *bit)
{
 unsigned zero=0;
 while(*bit<bytes*8 && zero<24) {
  unsigned x=(p[*bit/8]>>(7-(*bit%8)))&1;
  ++*bit;
  if(x)break;
  ++zero;
 }
 if(*bit>bytes*8 || zero>=24)return -1;
 unsigned value=1;
 for(unsigned i=0;i<zero;i++) {
  if(*bit>=bytes*8)return -1;
  value=(value<<1)|((p[*bit/8]>>(7-(*bit%8)))&1);
  ++*bit;
 }
 return (int)(value-1);
}
static int is_b_picture(const unsigned char *data,size_t size)
{
 for(size_t i=0;i+6<size;i++) {
  size_t prefix=0;
  if(data[i]==0 && data[i+1]==0 && data[i+2]==1)prefix=3;
  else if(i+4<size && data[i]==0 && data[i+1]==0 &&
          data[i+2]==0 && data[i+3]==1)prefix=4;
  if(!prefix)continue;
  size_t n=i+prefix;
  int type=data[n]&31;
  if(type!=1 && type!=5)continue;
  size_t len=size-n-1;
  if(len>32)len=32;
  unsigned bit=0;
  if(read_ue(data+n+1,len,&bit)<0)return 0;
  int slice=read_ue(data+n+1,len,&bit);
  return slice==1 || slice==6;
 }
 return 0;
}
static void pts_push(uint64_t pts)
{
 if(pending_count==PTS_WINDOW) {
  memmove(pending_pts,pending_pts+1,(PTS_WINDOW-1)*sizeof(pending_pts[0]));
  --pending_count;
 }
 pending_pts[pending_count++]=pts&((1ULL<<33)-1);
}
static int pts_take(uint64_t *pts)
{
 if(!pending_count)return 0;
 /* Decoder submission order differs from display order for B pictures.
  * Use the earliest presentation timestamp from AUs already submitted. */
 unsigned best=0;
 for(unsigned i=1;i<pending_count;i++)
  if(pts_difference(pending_pts[i],pending_pts[best])<0)best=i;
 *pts=pending_pts[best];
 if(best)st.reordered_pts++;
 memmove(pending_pts+best,pending_pts+best+1,
         (pending_count-best-1)*sizeof(pending_pts[0]));
 --pending_count;
 return 1;
}
/* Caller holds queue_mutex when called from the worker. */
static void clear_presentation(void)
{
 pending_count=0;
 picture_head=picture_tail=picture_count=0;
 last_shown_valid=0;
 last_shown_pts=0;
 st.decoded_queued=0;
}

static int avc_worker(SceSize args,void *argp);
void iptvita_preview_reset(void)
{
 /* Stop before freeing queues, decoder allocations or RGBA staging surfaces. */
 decoder_stop=1;
 if(decoder_thread>=0) {
  sceKernelWaitThreadEnd(decoder_thread,NULL,NULL);
  sceKernelDeleteThread(decoder_thread);
  decoder_thread=-1;
 }
 worker_enabled=0;
 if(queue_mutex<0)queue_mutex=sceKernelCreateMutex("iptvita_video_queue",0,0,NULL);
 queue_lock();
 while(count) {
  free(frames[head].data);
  head=(head+1)%FRAME_QUEUE;count--;
 }
 head=tail=0;total_bytes=0;due_at=0;playback_primed=0;previous_pts_valid=0;previous_pts=0;
 video_clock_valid=audio_master_active=0;video_base_pts=video_base_us=0;
 rebuffer_target=REBUFFER_MIN;last_underrun_us=last_target_decay_us=0;
 saw_picture=0;
 clear_presentation();
 queue_unlock();
 newest_pts=0;have_newest_pts=0;
 if(open)iptvita_avc_close(&dec);
 open=0;
 if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);texture=NULL;}
 texture_width=texture_height=0;
 for(unsigned i=0;i<DISPLAY_SLOTS;i++) {
  free(pictures[i].rgba);
  memset(&pictures[i],0,sizeof(pictures[i]));
 }
 picture_capacity=0;rgba_size=0;
 memset(&st,0,sizeof st);
 wait_for_idr=1;recovery_attempts=0;reference_frames=4;decoder_fatal=0;
 st.waiting_for_idr=1;
 st.buffering=1;
 st.buffer_target=START_FRAMES;
 decoder_stop=0;decoder_paused=0;last_decoder_us=0;
 decoder_thread=sceKernelCreateThread("iptvita_avc_worker",avc_worker,
                                     0x10000100,192*1024,0,0,NULL);
 if(decoder_thread>=0) {
  if(sceKernelStartThread(decoder_thread,0,NULL)>=0)worker_enabled=1;
  else {sceKernelDeleteThread(decoder_thread);decoder_thread=-1;}
 }
 st.worker_running=worker_enabled;
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
  /* Worker owns the codec and CPU staging surfaces. Never call vita2d here.
   * The UI creates/recreates the display texture on its own thread. */
  if(rgba_size<needed || st.width!=w || st.height!=h) {
   queue_lock();
   clear_presentation();
   queue_unlock();
   for(unsigned i=0;i<DISPLAY_SLOTS;i++){
    free(pictures[i].rgba);
    memset(&pictures[i],0,sizeof(pictures[i]));
   }
   picture_capacity=0;rgba_size=0;
  }
  if(!picture_capacity) {
   for(unsigned i=0;i<DISPLAY_SLOTS;i++) {
    pictures[i].rgba=(unsigned char*)malloc(needed);
    if(!pictures[i].rgba)break;
    picture_capacity++;
   }
   if(picture_capacity)rgba_size=needed;
  }
  if(picture_capacity<1) {
   st.decode_error=-31;
   iptvita_avc_close(&dec);
   for(unsigned i=0;i<DISPLAY_SLOTS;i++) {
    free(pictures[i].rgba);
    pictures[i].rgba=NULL;
   }
   picture_capacity=0;rgba_size=0;
   return;
  }
  st.decoded_capacity=picture_capacity;
  st.width=w;st.height=h;
  open=1;
 }
 if(picture_count>=picture_capacity)return;
 st.submitted++;
 st.decode_calls++;
 if(has_pts)pts_push(pts);
 uint64_t actual_pts=0;
 int actual_has_pts=0;
 DisplayPicture *picture=&pictures[picture_tail];
 int rc=iptvita_avc_decode(&dec,data,size,pts,has_pts,picture->rgba,rgba_size,
                           &actual_pts,&actual_has_pts);
 if(rc<0) {
  /* Bad AUs may be rejected before contributing a decoded picture. */
  if(has_pts && pending_count) --pending_count;
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
   queue_lock();clear_presentation();queue_unlock();
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
   clear_presentation();
   while(count) {
    free(frames[head].data);
    head=(head+1)%FRAME_QUEUE;
    count--;
   }
   total_bytes=0;tail=head;st.queued=0;st.queued_bytes=0;
   queue_unlock();
  }
  return;
 }
 wait_for_idr=0;
 st.waiting_for_idr=0;
 st.decode_error=0;
 if(rc==0)return;
 /* AVC may return the output for an earlier AU. The hardware produces
  * decoded pictures in its display sequence; the caller only schedules
  * presentation after the output exists. */
 int fallback_has_pts=pts_take(&picture->pts);
 picture->has_pts=fallback_has_pts;
 if(actual_has_pts) {
  picture->pts=actual_pts;
  picture->has_pts=1;
  st.hardware_pts++;
 }else if(fallback_has_pts)st.estimated_pts++;
 picture->ready_at=clock_us();
 queue_lock();
 picture_tail=(picture_tail+1)%picture_capacity;
 picture_count++;
 st.decoded_queued=picture_count;
 queue_unlock();
 st.decoder_phase=3;
}
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key)
{
 if(!data || !size || size>IPTVITA_H264_AU_MAX || decoder_fatal)return;
 queue_lock();
 /* Preserve B-pictures with PTS earlier than the reference P-picture
  * sent before them. The hardware needs ALL reference and dependent AUs. */
 if(has_pts) {
  if(have_newest_pts && pts<newest_pts) {
   uint64_t behind=newest_pts-pts;
   if(behind>90000ULL*15 && key) {
    newest_pts=pts;
    previous_pts_valid=0;
    /* A discontinuity should reset the presentation clock. */
    video_clock_valid=audio_master_active=0;
   }
  }
  if(!have_newest_pts || pts>newest_pts)newest_pts=pts;
  have_newest_pts=1;
 }
 if(key)st.keyframes++;
 if(is_b_picture(data,size))st.b_frames++;
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
 st.queued_bytes=(unsigned)total_bytes;
 if(st.queued_bytes>st.peak_queued_bytes)st.peak_queued_bytes=st.queued_bytes;
 unsigned goal=playback_primed?rebuffer_target:(saw_picture?rebuffer_target:START_FRAMES);
 st.buffer_target=goal;
 st.buffer_percent=count>=goal?100:(int)(count*100u/goal);
 queue_unlock();
}
static void retire_picture(void)
{
 if(!picture_count)return;
 picture_head=(picture_head+1)%picture_capacity;
 picture_count--;
 st.decoded_queued=picture_count;
}
static void present_picture(void)
{
 if(!picture_count || !texture)return;
 DisplayPicture *picture=&pictures[picture_head];
 unsigned char *dst=(unsigned char*)vita2d_texture_get_datap(texture);
 unsigned dst_stride=vita2d_texture_get_stride(texture);
 unsigned src_stride=(unsigned)((st.width+15)&~15)*4u;
 if(!dst || dst_stride<(unsigned)st.width*4u) {
  st.decode_error=-32;retire_picture();return;
 }
 for(int y=0;y<st.height;y++)
  memcpy(dst+(size_t)y*dst_stride,picture->rgba+(size_t)y*src_stride,
         (size_t)st.width*4u);
 st.decoded++;
 saw_picture=1;
 st.last_output_us=clock_us();
 if(picture->has_pts){last_shown_pts=picture->pts;last_shown_valid=1;}
 retire_picture();
}
static int presentation_due(unsigned long long now)
{
 if(!picture_count)return 0;
 DisplayPicture *picture=&pictures[picture_head];
 if(!picture->has_pts)return !due_at || now>=due_at;
 uint64_t audio_pts=0;
 if(iptvita_audio_clock(&audio_pts)) {
  int64_t delta=pts_difference(picture->pts,audio_pts);
  if(delta>-90000LL*8 && delta<90000LL*8) {
   st.av_sync=1;
   audio_master_active=1;
   if(delta>4500LL && now-picture->ready_at<1000000ULL)return 0;
   return 1;
  }
 }
 st.av_sync=0;audio_master_active=0;
 if(!video_clock_valid) {
  video_base_pts=picture->pts;
  video_base_us=now;
  video_clock_valid=1;
  return 1;
 }
 int64_t delta=pts_difference(picture->pts,video_base_pts);
 if(delta < -90000LL*3 || delta > 90000LL*30) {
  video_base_pts=picture->pts;video_base_us=now;return 1;
 }
 if(delta>0) {
  unsigned long long due=video_base_us+(unsigned long long)delta*100ULL/9ULL;
  if(due>now && due-now<1000000ULL && now-picture->ready_at<1000000ULL)return 0;
 }
 return 1;
}

/* The only consumer of the encoded H.264 queue. The renderer is never
 * required to feed the codec, and does not call sceAvcdecDecode. */
static int avc_worker(SceSize args,void *argp)
{
 (void)args;(void)argp;
 while(!decoder_stop) {
  EncodedFrame item={0};
  int got=0;
  queue_lock();
  unsigned long long now=clock_us();
  if(!decoder_paused && !decoder_fatal &&
     (!picture_capacity || picture_count<picture_capacity)) {
   if(!count && !picture_count) {
    if(playback_primed && saw_picture) {
     if(!last_underrun_us || now-last_underrun_us<15000000ULL) {
      if(rebuffer_target<REBUFFER_MAX)rebuffer_target+=REBUFFER_STEP;
     }
     last_underrun_us=now;
     last_target_decay_us=now;
     st.buffer_events++;
    }
    playback_primed=0;
    st.buffering=1;
    st.buffer_target=saw_picture?rebuffer_target:START_FRAMES;
    st.buffer_percent=0;
   }else if(count) {
    if(!playback_primed) {
     unsigned goal=saw_picture?rebuffer_target:START_FRAMES;
     st.buffering=1;
     st.buffer_target=goal;
     st.buffer_percent=count>=goal?100:(int)(count*100u/goal);
     if(count>=goal || total_bytes>=QUEUE_BYTES/2) {
      playback_primed=1;
      st.buffering=0;
      previous_pts_valid=0;
      video_clock_valid=audio_master_active=0;
      last_target_decay_us=now;
     }
    }
    if(playback_primed) {
     if(saw_picture && rebuffer_target>REBUFFER_MIN &&
        now>=last_target_decay_us &&
        now-last_target_decay_us>STABLE_DECAY_US) {
      rebuffer_target-=REBUFFER_STEP;
      last_target_decay_us=now;
     }
     item=frames[head];
     head=(head+1)%FRAME_QUEUE;count--;
     total_bytes-=item.size;
     st.queued=count;
     st.queued_bytes=(unsigned)total_bytes;
     st.buffer_target=rebuffer_target;
     st.buffer_percent=count>=rebuffer_target?100:
                            (int)(count*100u/rebuffer_target);
     got=1;
    }
   }
  }
  queue_unlock();
  if(!got) {
   sceKernelDelayThread(2000);
   continue;
  }
  if(decoder_stop) {free(item.data);break;}
  decode_frame(item.data,item.size,item.pts,item.has_pts,item.key);
  free(item.data);
  last_decoder_us=clock_us();
 }
 sceKernelExitThread(0);
 return 0;
}
/* UI thread: GXM/vita2d objects never cross into the AVC worker. */
void iptvita_preview_tick(void)
{
 unsigned width=0,height=0;
 queue_lock();
 if(picture_count && st.width>0 && st.height>0) {
  width=(unsigned)st.width;
  height=(unsigned)st.height;
 }
 queue_unlock();
 if(width && (!texture || width!=texture_width || height!=texture_height)) {
  if(texture) {
   vita2d_wait_rendering_done();
   vita2d_free_texture(texture);
   texture=NULL;
   texture_width=texture_height=0;
  }
  texture=vita2d_create_empty_texture(width,height);
  if(texture){texture_width=width;texture_height=height;}
 }
 if(!texture)return;
 queue_lock();
 unsigned long long now=clock_us();
 if(picture_count && presentation_due(now)) {
  DisplayPicture *p=&pictures[picture_head];
  uint64_t audio_pts=0;
  int too_old=p->has_pts && iptvita_audio_clock(&audio_pts) &&
              pts_difference(p->pts,audio_pts)<-18000LL;
  int backwards=p->has_pts && last_shown_valid &&
                pts_difference(p->pts,last_shown_pts)<-4500LL;
  if(too_old || backwards) {
   st.dropped++;
   st.late_pictures++;
   retire_picture();
  } else {
   int no_pts=!p->has_pts;
   present_picture();
   if(no_pts)due_at=now+33333ULL;
  }
 }else if(picture_count)st.picture_waits++;
 queue_unlock();
}
void iptvita_preview_pause(int on)
{
 queue_lock();
 decoder_paused=on?1:0;
 if(!on) {
  due_at=0;
  video_clock_valid=audio_master_active=0;
  previous_pts_valid=0;
 }
 queue_unlock();
}
void iptvita_preview_resume(void){iptvita_preview_pause(0);}
IptvitaPreviewStats iptvita_preview_stats(void)
{
 queue_lock();
 IptvitaPreviewStats snapshot=st;
 snapshot.worker_running=worker_enabled && !decoder_stop;
 queue_unlock();
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
