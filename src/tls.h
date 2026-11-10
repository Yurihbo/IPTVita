#ifndef IPTVITA_TLS_H
#define IPTVITA_TLS_H
#include <stddef.h>
/* HTTPS GET into a bounded caller buffer. Returns bytes or a negative error. */
int iptvita_tls_get(const char *url, const unsigned char *ca, size_t ca_len,
                    unsigned char *out, size_t capacity, int *http_code);
#endif
