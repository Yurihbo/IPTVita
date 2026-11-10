#ifndef IPTVITA_VIDEO_PREVIEW_H
#define IPTVITA_VIDEO_PREVIEW_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
 int width,height,decode_error;
 unsigned decoded,submitted,keyframes,dropped,recoveries,queued;
 unsigned long long last_output_us;
 int buffer_percent;
 int waiting_for_idr;
 int fatal;
 int decoder_phase; /* 0 idle, 1 init failure, 2 decode, 3 output seen */
} IptvitaPreviewStats;
void iptvita_preview_reset(void);
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key);
IptvitaPreviewStats iptvita_preview_stats(void);
/* Consume at most one compressed access unit per video tick. */
void iptvita_preview_tick(void);
void iptvita_preview_draw(int x,int y,int width,int height);
#endif
