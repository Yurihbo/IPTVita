#include <psp2/kernel/rng.h>
#include <stddef.h>
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
 (void)data;
 if(olen)*olen=0;
 if(!output || !olen)return -1;
 while(len) {
  unsigned int part=len>256?256:(unsigned int)len;
  int result=sceKernelGetRandomNumber(output,part);
  if(result<0)return result;
  output+=part;
  *olen+=part;
  len-=part;
 }
 return 0;
}
