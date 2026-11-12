#ifndef IPTVITA_VIDEO_PREVIEW_H
#define IPTVITA_VIDEO_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
 int width,height,decode_error;
 unsigned decoded,submitted,keyframes,dropped,recoveries,queued;
 unsigned buffer_events,buffer_target;
 unsigned b_frames,reordered_pts,late_pictures;
 unsigned hardware_pts,estimated_pts;
 unsigned decode_calls,picture_waits;
 unsigned queued_bytes,peak_queued_bytes;
 unsigned queue_capacity,queue_limit_mib,queue_expanded;
 unsigned queue_expansions,queue_growth_denied;
 unsigned decoded_queued,decoded_capacity;
 unsigned video_fps,decode_fps,video_fps_samples;
 int worker_running;
 unsigned long long last_output_us;
 int buffer_percent;
 int buffering; /* 1 while awaiting enough video frames to start/resume */
 int av_sync; /* 1: audio master clock; 0: local video clock */
 int waiting_for_idr;
 int fatal;
 int decoder_phase; /* 0 idle, 1 init failure, 2 decode, 3 output seen */
} IptvitaPreviewStats;
void iptvita_preview_reset(void);
int iptvita_preview_start(void);
void iptvita_preview_pause(int on);
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key);
IptvitaPreviewStats iptvita_preview_stats(void);
/* UI consumes at most one ready picture per screen tick.
 * The independent H264 worker consumes compressed access units. */
void iptvita_preview_tick(void);
void iptvita_preview_resume(void);
void iptvita_preview_draw(int x,int y,int width,int height,int fill);
#endif
