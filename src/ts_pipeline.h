#ifndef IPTVITA_TS_PIPELINE_H
#define IPTVITA_TS_PIPELINE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
 unsigned packets, sync_errors, continuity_errors;
 unsigned pat_packets, pmt_packets, video_packets, audio_packets;
 int pmt_pid, video_pid, audio_pid, video_type, audio_type;
 unsigned char counters[8192];
 unsigned pes_video_starts, pes_audio_starts;
 uint64_t last_video_pts, last_audio_pts;
 unsigned video_pts_valid, audio_pts_valid;
} IptvitaTsPipeline;
void iptvita_ts_init(IptvitaTsPipeline *p);
int iptvita_ts_feed(IptvitaTsPipeline *p,const unsigned char *data,size_t size);
#endif
