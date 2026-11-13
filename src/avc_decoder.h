#ifndef IPTVITA_AVC_DECODER_H
#define IPTVITA_AVC_DECODER_H
#include <psp2/videodec.h>
#include <psp2/types.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
 SceAvcdecCtrl ctrl;
 SceUID frame_uid;
 SceUID es_uid;
 SceUID picture_uid[8];
 unsigned char picture_mapped[8];
 unsigned char *es_data,*picture_data[8];
 unsigned picture_count,slot;
 size_t es_capacity;
 int library_open,decoder_open;
 int disable_pts, pts_retries;
 int width,height;
} IptvitaAvcDecoder;
/* First native H.264 decoder backend: supports up to 1280x720 using
 * the public SceVideodec API. */
int iptvita_avc_open(IptvitaAvcDecoder *d,int width,int height,int references);
/* Feed one Annex-B H264 access unit and collect up to two decoded pictures
 * in presentation order. The public Vita AVCDEC may release TWO pictures for
 * one AU (especially when flushing a reorder queue at IDR). Each rgba[k]
 * is independent and must fit aligned_width * aligned_height * 4 bytes.
 * max_pictures must be 1 or 2, and each pointer must be valid.
 * Returns 0..max_pictures picture outputs, negative on SDK failure.
 * Picture PTS values belong to the corresponding output, not the input AU. */
int iptvita_avc_decode(IptvitaAvcDecoder *d,const unsigned char *au,size_t size,
                       uint64_t pts,int has_pts,void *rgba[2],size_t rgba_capacity,
                       unsigned max_pictures,uint64_t picture_pts[2],
                       int picture_has_pts[2]);
void iptvita_avc_close(IptvitaAvcDecoder *d);
#endif
