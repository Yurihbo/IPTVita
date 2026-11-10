#ifndef IPTVITA_H264_SPS_H
#define IPTVITA_H264_SPS_H
#include <stddef.h>
/* Read dimensions from H.264 Annex-B SPS, without copying implementation
 * from another IPTV player. Returns 0 on success, negative on invalid SPS. */
int iptvita_h264_dimensions(const unsigned char *data,size_t size,int *width,int *height);
#endif
