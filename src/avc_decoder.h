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
 * the public SceVideodec API. Image submission is added next. */
int iptvita_avc_open(IptvitaAvcDecoder *d,int width,int height,int references);
void iptvita_avc_close(IptvitaAvcDecoder *d);
#endif
