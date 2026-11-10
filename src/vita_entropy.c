#include <stddef.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>

/* VitaSDK newlib entropy, also used by the Vita IPTV reference architecture.
 * getentropy accepts a maximum of 256 bytes per call. */
extern int getentropy(void *buffer, size_t length);

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
 (void)data;
 if (!olen) return -1;
 *olen=0;
 if (!output) return -1;
 while (*olen < len) {
  size_t part=len-*olen;
  if (part>256) part=256;
  if (getentropy(output+*olen,part)!=0) return -1;
  *olen+=part;
 }
 return 0;
}

uint64_t mbedtls_ms_time(void)
{
 return (uint64_t)(sceKernelGetProcessTimeWide()/1000);
}
