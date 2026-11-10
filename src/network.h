#ifndef IPTVITA_NETWORK_H
#define IPTVITA_NETWORK_H
/* Download is performed in a worker thread. State: 0 idle, 1 running,
   2 success, -1 network/init error, -2 HTTP error, -3 too large. */
int iptvita_network_start(const char *url);
int iptvita_network_state(void);
const char *iptvita_network_cache_path(void);
void iptvita_network_reset(void);
#endif
