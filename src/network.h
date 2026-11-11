#ifndef IPTVITA_NETWORK_H
#define IPTVITA_NETWORK_H
/* Download is performed in a worker thread. State: 0 idle, 1 running,
   2 success, -1 network/init error, -2 HTTP error, -3 too large. */
int iptvita_network_start(const char *url);
int iptvita_network_state(void);
int iptvita_network_error_code(void);
int iptvita_network_http_status(void);
const char *iptvita_network_cache_path(void);
void iptvita_network_reset(void);
/* One optional, independent segment cache: never writes online.m3u while
 * the demux worker is reading it. A match is consumed transparently by
 * iptvita_network_start(), and a miss falls back to normal HTTP. */
int iptvita_network_prefetch(const char *url);
int iptvita_network_prefetch_ready(const char *url);
void iptvita_network_prefetch_clear(void);
unsigned iptvita_network_prefetch_hits(void);
#endif
