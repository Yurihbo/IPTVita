#ifndef IPTVITA_H264_AU_H
#define IPTVITA_H264_AU_H
#include <stddef.h>
#include <stdint.h>
#define IPTVITA_H264_AU_MAX (2u*1024u*1024u)
typedef void (*IptvitaAuSink)(void *user,const unsigned char *data,size_t size,uint64_t pts,int has_pts,int keyframe);
typedef struct {
 unsigned char *buffer;
 size_t used;
 int has_vcl,has_idr,has_pts;
 uint64_t pts;
 unsigned units,frames,dropped;
 /* Keep the most recent codec parameter sets across HLS/PES boundaries. */
 unsigned char sps[1024],pps[1024];
 size_t sps_size,pps_size;
 IptvitaAuSink sink;
 void *user;
} IptvitaH264Au;
int iptvita_h264_au_open(IptvitaH264Au *a,IptvitaAuSink sink,void *user);
void iptvita_h264_au_pes(void *user,const unsigned char *data,size_t size,uint64_t pts,int has_pts);
void iptvita_h264_au_flush(IptvitaH264Au *a);
void iptvita_h264_au_close(IptvitaH264Au *a);
#endif
