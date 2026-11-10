#ifndef IPTVITA_HLS_H
#define IPTVITA_HLS_H
#include <stddef.h>
#define IPTVITA_HLS_URL_MAX 8192
typedef struct {
 int is_hls, is_master, encrypted, segments, variants;
 char first_media[IPTVITA_HLS_URL_MAX];
} IptvitaHlsInfo;
/* Inspect HLS text without downloading or decoding video. */
int iptvita_hls_inspect(const char *text, const char *base, IptvitaHlsInfo *out);
#endif
