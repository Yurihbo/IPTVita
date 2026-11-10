#ifndef IPTVITA_PES_ASSEMBLER_H
#define IPTVITA_PES_ASSEMBLER_H
#include <stddef.h>
#include <stdint.h>
#define IPTVITA_PES_MAX (2u*1024u*1024u)
typedef void (*IptvitaPesSink)(void *user,const unsigned char *data,size_t size,uint64_t pts,int has_pts);
typedef struct {
 unsigned char *data;
 size_t used;
 int pid,active,has_pts;
 uint64_t pts;
 unsigned emitted,dropped;
 IptvitaPesSink sink;
 void *user;
} IptvitaPesAssembler;
int iptvita_pes_open(IptvitaPesAssembler *a,int pid,IptvitaPesSink sink,void *user);
void iptvita_pes_push(IptvitaPesAssembler *a,const unsigned char *ts,size_t size);
void iptvita_pes_flush(IptvitaPesAssembler *a);
void iptvita_pes_close(IptvitaPesAssembler *a);
#endif
