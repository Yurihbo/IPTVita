#include "hls_crypto.h"
#include <mbedtls/aes.h>
#include <string.h>
#include <limits.h>

int iptvita_hls_decrypt(unsigned char *buffer,size_t size,
                        const unsigned char key[16],const unsigned char iv[16])
{
 if(!buffer || !key || !iv || !size || (size&15) || size>INT_MAX)return -1;
 mbedtls_aes_context aes;
 mbedtls_aes_init(&aes);
 unsigned char chain[16];
 memcpy(chain,iv,16);
 int rc=mbedtls_aes_setkey_dec(&aes,key,128);
 if(rc==0)rc=mbedtls_aes_crypt_cbc(&aes,MBEDTLS_AES_DECRYPT,size,chain,buffer,buffer);
 mbedtls_aes_free(&aes);
 memset(chain,0,sizeof(chain));
 if(rc!=0)return -2;
 /* HLS AES-128 uses PKCS7 block padding, including a full block when needed. */
 unsigned pad=buffer[size-1];
 if(!pad || pad>16 || pad>size)return -3;
 for(size_t i=size-pad;i<size;i++)if(buffer[i]!=pad)return -3;
 return (int)(size-pad);
}
