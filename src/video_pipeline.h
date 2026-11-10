#ifndef IPTVITA_VIDEO_PIPELINE_H
#define IPTVITA_VIDEO_PIPELINE_H
#include "ts_pipeline.h"
#include "pes_assembler.h"
#include "h264_au.h"
typedef struct {
 IptvitaTsPipeline transport;
 IptvitaPesAssembler video_pes;
 IptvitaH264Au video_au;
 int video_pid;
 int active;
 IptvitaAuSink sink;
 void *user;
} IptvitaVideoPipeline;
/* TS input -> PAT/PMT -> video PES -> AVC access units callback.
 * Feed complete 188-byte TS packets; future network reader will use this. */
int iptvita_video_open(IptvitaVideoPipeline *v,IptvitaAuSink sink,void *user);
int iptvita_video_feed(IptvitaVideoPipeline *v,const unsigned char *ts,size_t size);
void iptvita_video_close(IptvitaVideoPipeline *v);
#endif
