#include "video_preview.h"
#include "avc_decoder.h"
#include "h264_sps.h"
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
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
static unsigned long long fps_window_us;
static unsigned fps_decode_base,fps_present_base;
static IptvitaPreviewStats st;
static int wait_for_idr=1, recovery_attempts=0, reference_frames=4;
static int decoder_fatal;
static uint64_t newest_pts;
static int have_newest_pts;
/* Capacity is an upper bound, not an amount reserved upfront.
 * Start with a 24-MiB / 1800-AU budget and expand only under sustained
 * pressure with sufficient free application memory. */
#define FRAME_QUEUE 3600
#define QUEUE_BASE_COUNT 1800u
#define QUEUE_BASE_BYTES (24u*1024u*1024u)
#define QUEUE_BIG_COUNT 3600u
#define QUEUE_BIG_BYTES (40u*1024u*1024u)
#define QUEUE_GROW_HEADROOM (32u*1024u*1024u)
#define QUEUE_SHRINK_US 45000000ULL
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
static unsigned queue_count_limit=QUEUE_BASE_COUNT;
static size_t queue_bytes_limit=QUEUE_BASE_BYTES;
static unsigned long long queue_big_since;
static unsigned long long queue_check_at;
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
/* Wall-clock pacing is independent of how far audio has advanced.
 * Recover lateness by dropping stale PRESENTATIONS, never by running
 * surviving animation frames at the Vita's ~60-Hz render cadence. */
static unsigned long long last_picture_us;
static unsigned long long next_picture_due_us;
static unsigned long long last_catchup_skip_us;
static unsigned long long rebuffer_since_us;
/* Live playback must never deadlock at a full RGBA presentation queue. */
#define PICTURE_STALL_RESCUE_US 1250000ULL
#define REBUFFER_MAX_WAIT_US 5000000ULL
static unsigned frame_interval_us=33333u;
static uint64_t last_decoded_picture_pts;
static int last_decoded_picture_valid;
static unsigned long long clock_us(void){return (unsigned long long)sceKernelGetProcessTimeWide();}
/* sceKernelGetFreeMemorySize reports available system pool budgets. This is
 * a conservative admission check, not a reservation; every AU is still
 * allocated individually and malloc failure is handled without crashing. */
static int queue_memory_allows_growth(void)
{
 SceKernelFreeMemorySizeInfo free_mem;
 memset(&free_mem,0,sizeof free_mem);
 free_mem.size=sizeof free_mem;
 if(sceKernelGetFreeMemorySize(&free_mem)<0)return 0;
 /* Increasing the ceiling by 16 MiB must still leave 32 MiB for decoding,
  * audio, graphics and network. */
 return (uint64_t)free_mem.size_user >=
   (uint64_t)(QUEUE_BIG_BYTES-QUEUE_BASE_BYTES)+QUEUE_GROW_HEADROOM;
}
/* These helpers run with queue_mutex held. */
static void queue_publish_limits(void)
{
 st.queue_capacity=queue_count_limit;
 st.queue_limit_mib=(unsigned)(queue_bytes_limit/(1024u*1024u));
 st.queue_expanded=queue_count_limit>QUEUE_BASE_COUNT;
}
static int queue_maybe_expand_locked(size_t incoming)
{
 unsigned long long now=clock_us();
 if(queue_count_limit==QUEUE_BIG_COUNT)return 1;
 int pressure=count+1>=queue_count_limit*3u/4u ||
              total_bytes+incoming>=queue_bytes_limit*3u/4u;
 if(!pressure)return 0;
 if(queue_check_at && now<queue_check_at)return 0;
 queue_check_at=now+5000000ULL;
 if(!queue_memory_allows_growth()){
  st.queue_growth_denied++;
  return 0;
 }
 queue_count_limit=QUEUE_BIG_COUNT;
 queue_bytes_limit=QUEUE_BIG_BYTES;
 queue_big_since=now;
 st.queue_expansions++;
 queue_publish_limits();
 return 1;
}
static void queue_maybe_shrink_locked(void)
{
 if(queue_count_limit!=QUEUE_BIG_COUNT)return;
 unsigned long long now=clock_us();
 /* Shrink the limit, not any queued data. Let pre-existing video drain. */
 if(now>=queue_big_since && now-queue_big_since>=QUEUE_SHRINK_US &&
    count<QUEUE_BASE_COUNT/2 && total_bytes<QUEUE_BASE_BYTES/2) {
  queue_count_limit=QUEUE_BASE_COUNT;
  queue_bytes_limit=QUEUE_BASE_BYTES;
  queue_check_at=now+5000000ULL;
  queue_publish_limits();
 }
}
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
 last_picture_us=0;
 next_picture_due_us=0;
 last_catchup_skip_us=0;
 rebuffer_since_us=0;
 last_decoded_picture_pts=0;
 last_decoded_picture_valid=0;
 frame_interval_us=33333u;
 st.cadence_ms=33;
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
 head=tail=0;total_bytes=0;
 queue_count_limit=QUEUE_BASE_COUNT;queue_bytes_limit=QUEUE_BASE_BYTES;
 queue_big_since=queue_check_at=0;
 due_at=0;playback_primed=0;rebuffer_since_us=0;
 previous_pts_valid=0;previous_pts=0;
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
 queue_publish_limits();
 decoder_stop=0;decoder_paused=0;last_decoder_us=0;
 fps_window_us=0;fps_decode_base=fps_present_base=0;
}
int iptvita_preview_start(void)
{
 if(decoder_thread>=0)return worker_enabled?0:-1;
 decoder_stop=0;
 decoder_thread=sceKernelCreateThread("iptvita_avc_worker",avc_worker,
                                     0x10000100,192*1024,0,0,NULL);
 if(decoder_thread>=0) {
  if(sceKernelStartThread(decoder_thread,0,NULL)>=0)worker_enabled=1;
  else {sceKernelDeleteThread(decoder_thread);decoder_thread=-1;}
 }
 st.worker_running=worker_enabled;
 return worker_enabled?0:-1;
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
  queue_lock();
  st.decoded_capacity=picture_capacity;
  st.width=w;st.height=h;
  open=1;
  queue_unlock();
 }
 /* A decoder call can return two pictures. Make BOTH RGBA slots available
  * before feeding the AU, or the public AVCDEC may retain/delay the second.
  * A low-memory configuration with just one staging slot uses the previous
  * single-output behavior rather than allocating unbounded video surfaces. */
 unsigned available=picture_capacity-picture_count;
 unsigned requested=available>=2?2:1;
 void *rgba_outputs[2]={
  pictures[picture_tail].rgba,
  requested==2?pictures[(picture_tail+1)%picture_capacity].rgba:NULL
 };
 uint64_t actual_pts[2]={0,0};
 int actual_has_pts[2]={0,0};
 st.submitted++;
 st.decode_calls++;
 if(has_pts)pts_push(pts);
 int rc=iptvita_avc_decode(&dec,data,size,pts,has_pts,
                            rgba_outputs,rgba_size,requested,
                            actual_pts,actual_has_pts);
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
 if(rc==0){st.avc_zero_output++;return;}
 if(rc==2)st.avc_dual_output++;
 st.avc_pictures+=(unsigned)rc;
 /* Publish the two pictures separately in decoder presentation order.
  * Never replace an already-ready RGBA surface, never infer both output PTS
  * from the same compressed AU. The original VitaIPTV also publishes each
  * picture separately when the public AVCDEC returns a pair. */
 for(int i=0;i<rc;i++) {
  DisplayPicture *picture=&pictures[picture_tail];
  int fallback_has_pts=pts_take(&picture->pts);
  picture->has_pts=fallback_has_pts;
  if(actual_has_pts[i]) {
   picture->pts=actual_pts[i];
   picture->has_pts=1;
   st.hardware_pts++;
  }else if(fallback_has_pts)st.estimated_pts++;
  picture->ready_at=clock_us();
  queue_lock();
  /* Use decoder output timestamps to learn natural frame cadence.
   * Do not train from UI frames: rendering may drop pictures for A/V sync. */
  if(picture->has_pts) {
   if(last_decoded_picture_valid) {
    int64_t ticks=pts_difference(picture->pts,last_decoded_picture_pts);
    if(ticks>=1500 && ticks<=5400) {
     unsigned sample=(unsigned)(ticks*1000000LL/90000LL);
     if(sample>=16000u && sample<=60000u) {
      frame_interval_us=(frame_interval_us*3u+sample+2u)/4u;
      st.cadence_ms=(frame_interval_us+500u)/1000u;
     }
    }else if(ticks< -90000LL || ticks>90000LL) {
     last_decoded_picture_valid=0;
     next_picture_due_us=0;
     st.pts_discontinuities++;
    }
   }
   last_decoded_picture_pts=picture->pts;
   last_decoded_picture_valid=1;
  }
  picture_tail=(picture_tail+1)%picture_capacity;
  picture_count++;
  st.decoded_queued=picture_count;
  queue_unlock();
 }
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
 /* Growth is demand-driven; never allocate a fixed 40 MiB slab. */
 queue_maybe_expand_locked(size);
 if(count>=queue_count_limit || size>queue_bytes_limit-total_bytes){
  st.dropped++;queue_unlock();return;
 }
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
/* Decoder order is not always display order. An H.264 stream with
 * B-frames may publish PTS 100, 166, 133: showing each ready FIFO picture
 * creates apparent backwards motion or forces the 133 frame to be dropped.
 * Reorder only the already-published slots (up to three). Never touch a
 * free surface on which AVCDEC may currently be writing. */
static void promote_earliest_picture_locked(void)
{
 if(picture_count<2 || picture_capacity<2)return;
 unsigned best=picture_head;
 for(unsigned i=1;i<picture_count;i++) {
  unsigned slot=(picture_head+i)%picture_capacity;
  if(!pictures[slot].has_pts || !pictures[best].has_pts)continue;
  int64_t delta=pts_difference(pictures[slot].pts,pictures[best].pts);
  if(delta<0 && delta> -90000LL*2)best=slot;
 }
 if(best!=picture_head) {
  DisplayPicture tmp=pictures[picture_head];
  pictures[picture_head]=pictures[best];
  pictures[best]=tmp;
  st.presentation_reorders++;
 }
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
 last_picture_us=st.last_output_us;
 /* Phase-locked deadline: preserve fractional 24/25/30-fps timing across
  * a 60-Hz render loop. Do NOT restart a full frame interval after each
  * slightly late VBlank, which was quantizing 24fps to ~20fps. */
 if(!next_picture_due_us ||
    st.last_output_us>next_picture_due_us+2ULL*frame_interval_us)
  next_picture_due_us=st.last_output_us+frame_interval_us;
 else
  next_picture_due_us+=frame_interval_us;
 if(picture->has_pts){last_shown_pts=picture->pts;last_shown_valid=1;}
 retire_picture();
}
/* Frame-rate limiter for images already approved by audio PTS.
 * Audio is a master clock, not an instruction to render all late pictures
 * as quickly as possible. Use presentation PTS intervals when credible:
 * ~3750 ticks = 24 FPS, 3600 = 25, 3000 = 30.
 * Gaps larger than one source frame indicate dropped pictures, not an
 * intentional long frame, so preserve the most recent learned cadence. */
static int cadence_due(const DisplayPicture *picture,unsigned long long now)
{
 (void)picture;
 if(next_picture_due_us && now<next_picture_due_us) {
  st.cadence_waits++;
  return 0;
 }
 return 1;
}
static int presentation_due(unsigned long long now)
{
 if(!picture_count)return 0;
 DisplayPicture *picture=&pictures[picture_head];
 /* The cadence deadline already governs streams without picture PTS. */
 if(!picture->has_pts)return 1;
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
     (!picture_capacity ||
      (picture_capacity>=2 ? picture_count+2<=picture_capacity :
                             picture_count<picture_capacity))) {
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
    if(!rebuffer_since_us)rebuffer_since_us=now;
    st.buffering=1;
    st.buffer_target=saw_picture?rebuffer_target:START_FRAMES;
    st.buffer_percent=0;
   }else if(count) {
    if(!playback_primed) {
     unsigned goal=saw_picture?rebuffer_target:START_FRAMES;
     st.buffering=1;
     st.buffer_target=goal;
     st.buffer_percent=count>=goal?100:(int)(count*100u/goal);
     if(!rebuffer_since_us)rebuffer_since_us=now;
     int enough=count>=goal || total_bytes>=queue_bytes_limit/2;
     /* The live playlist may stop producing segments before the adaptive
      * target is reached. Never withhold already-buffered pictures forever:
      * after a bounded wait, resume using the available AUs. No fabricated
      * input is emitted if the network supplies nothing. */
     int timed_out=saw_picture && count>=2 &&
         now>=rebuffer_since_us &&
         now-rebuffer_since_us>=REBUFFER_MAX_WAIT_US;
     if(enough || timed_out) {
      if(timed_out && !enough)st.rebuffer_timeouts++;
      playback_primed=1;
      rebuffer_since_us=0;
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
     queue_maybe_shrink_locked();
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
 promote_earliest_picture_locked();
 /* Recover an A/V backlog in a single UI tick. Only discard already
  * decoded pictures; never drop compressed P/B reference AUs. When all
  * pictures are late, present the latest rather than freezing forever. */
 for(unsigned tries=0;tries<DISPLAY_SLOTS && picture_count;tries++) {
  DisplayPicture *p=&pictures[picture_head];
  /* If every RGBA slot is full and the displayed picture has not advanced
   * for over a second, presentation PTS may be stalled. One bounded rescue
   * releases a slot so the independent decoder can feed the next AU. */
  int full=picture_capacity>0 && picture_count>=picture_capacity;
  int old_picture=now>=p->ready_at &&
                  now-p->ready_at>=PICTURE_STALL_RESCUE_US;
  int no_progress=!st.last_output_us ||
                  (now>st.last_output_us &&
                   now-st.last_output_us>=PICTURE_STALL_RESCUE_US);
  int rescue=full && old_picture && no_progress;
  if(!rescue && !presentation_due(now)) {
   st.picture_waits++;
   break;
  }
  if(rescue) {
   st.stall_rescues++;
   video_clock_valid=0;
   audio_master_active=0;
   next_picture_due_us=0;
  }
  uint64_t audio_pts=0;
  int has_audio=p->has_pts && iptvita_audio_clock(&audio_pts);
  int64_t av_delta=has_audio?pts_difference(p->pts,audio_pts):0;
  st.av_delta_ms=has_audio?(int)(av_delta/90):0;
  /* Only trim meaningful (>500ms) persistent lag when another decoded
   * image is already available. At most one skip every 120ms prevents
   * rapid jump-cuts/animation fast-forward across consecutive UI ticks. */
  int too_old=has_audio && av_delta< -45000LL &&
              picture_count>1 &&
              (!last_catchup_skip_us ||
               now-last_catchup_skip_us>=120000ULL);
  int backwards=p->has_pts && last_shown_valid &&
                pts_difference(p->pts,last_shown_pts)<-4500LL;
  if(backwards || too_old) {
   st.dropped++;
   st.late_pictures++;
   if(too_old)last_catchup_skip_us=now;
   retire_picture();
   /* Do not drop several good pictures during a single screen tick. */
   break;
  }
  if(!rescue && !cadence_due(p,now))break;
  int no_pts=!p->has_pts;
  present_picture();
  if(no_pts)due_at=next_picture_due_us;
  break;
 }
 /* Measure real pictures presented and codec calls, not the screen capture
  * frame rate. A low OUT with high IN means post-decode timing pressure. */
 if(!fps_window_us){fps_window_us=now;fps_decode_base=st.decode_calls;fps_present_base=st.decoded;}
 else if(now>=fps_window_us && now-fps_window_us>=1000000ULL) {
  unsigned long long delta=now-fps_window_us;
  st.decode_fps=(unsigned)(((unsigned long long)(st.decode_calls-fps_decode_base)*1000000ULL)/delta);
  st.video_fps=(unsigned)(((unsigned long long)(st.decoded-fps_present_base)*1000000ULL)/delta);
  st.video_fps_samples++;
  fps_window_us=now;fps_decode_base=st.decode_calls;fps_present_base=st.decoded;
 }
 queue_unlock();
}
void iptvita_preview_pause(int on)
{
 queue_lock();
 decoder_paused=on?1:0;
 if(!on) {
  last_picture_us=0;
  next_picture_due_us=0;
  last_catchup_skip_us=0;
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
