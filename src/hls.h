#ifndef IPTVITA_HLS_H
#define IPTVITA_HLS_H
#include <stddef.h>
#define IPTVITA_HLS_URL_MAX 8192
typedef struct {
 int is_hls, is_master, encrypted, segments, variants;
 /* 0 none, 1 AES-128 full-segment encryption, 2 unsupported/DRM */
 int cipher_method;
 unsigned char iv[16];
 int iv_explicit;
 unsigned long long media_sequence;
 char key_url[IPTVITA_HLS_URL_MAX];
 /* Optional independent audio rendition advertised by an HLS master. */
 char audio_media[IPTVITA_HLS_URL_MAX];
 char first_media[IPTVITA_HLS_URL_MAX];
 /* Exactly one known successor for bounded parallel prefetch. Only safe for
  * unencrypted MPEG-TS, since AES keys/IVs can change between segments. */
 char next_media[IPTVITA_HLS_URL_MAX];
 unsigned long long next_sequence;
 int next_cipher_method;
} IptvitaHlsInfo;
/* Inspect HLS text without downloading or decoding video. */
int iptvita_hls_inspect(const char *text, const char *base, IptvitaHlsInfo *out);
/* Choose next segment after the previous media sequence rather than skipping ahead. */
int iptvita_hls_inspect_next_quality(const char *text,const char *base,IptvitaHlsInfo *out,
                             unsigned long long last_sequence,int have_previous,int prefer_hd);
int iptvita_hls_inspect_next(const char *text,const char *base,IptvitaHlsInfo *out,
                             unsigned long long last_sequence,int have_previous);
#endif
