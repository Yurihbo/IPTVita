#ifndef IPTVITA_AVC_DECODER_H
#define IPTVITA_AVC_DECODER_H
#include <psp2/videodec.h>
#include <psp2/types.h>
typedef struct {
 SceAvcdecCtrl ctrl;
 SceUID frame_uid;
 int library_open,decoder_open;
 int width,height;
} IptvitaAvcDecoder;
/* First native H.264 decoder backend: supports up to 1280x720 using
 * the public SceVideodec API. */
int iptvita_avc_open(IptvitaAvcDecoder *d,int width,int height,int references);
/* Decode one Annex-B access unit into an aligned RGBA8888 frame buffer.\n * rgba must hold at least aligned_width * aligned_height * 4 bytes.\n * Returns 1 if a picture was produced, 0 if buffered, negative SDK error. */\nint iptvita_avc_decode(IptvitaAvcDecoder *d,const unsigned char *au,size_t size,\n                       uint64_t pts,int has_pts,void *rgba,size_t rgba_capacity);\nvoid iptvita_avc_close(IptvitaAvcDecoder *d);
#endif
