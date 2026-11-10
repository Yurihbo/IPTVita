#ifndef IPTVITA_HLS_CRYPTO_H
#define IPTVITA_HLS_CRYPTO_H
#include <stddef.h>
/* Decrypt a complete standards-compliant AES-128/CBC HLS segment in place.
 * Returns plaintext bytes, negative code on invalid padding or input. */
int iptvita_hls_decrypt(unsigned char *buffer,size_t size,
                        const unsigned char key[16],const unsigned char iv[16]);
#endif
