#include "pes_assembler.h"
#include <stdlib.h>
#include <string.h>

/* Original bounded PES elementary-stream assembler for one MPEG-TS PID.
 * The sink receives Annex-B H264 payload bytes (or audio ES) with PTS.
 * No decoder or renderer is invoked at this stage. */
static void emit(IptvitaPesAssembler *a)
{
 if(a->active && a->used && a->sink) {
  a->sink(a->user,a->data,a->used,a->pts,a->has_pts);
  a->emitted++;
 }
 a->used=0;a->active=0;a->has_pts=0;
}
int iptvita_pes_open(IptvitaPesAssembler *a,int pid,IptvitaPesSink sink,void *user)
{
 if(!a || pid<0 || pid>8191)return -1;
 memset(a,0,sizeof(*a));
 a->data=(unsigned char*)malloc(IPTVITA_PES_MAX);
 if(!a->data)return -2;
 a->pid=pid;a->sink=sink;a->user=user;
 return 0;
}
static void start(IptvitaPesAssembler *a,const unsigned char **data,size_t *size)
{
 const unsigned char *p=*data;size_t n=*size;
 a->used=0;a->active=0;a->has_pts=0;
 if(n<9 || p[0] || p[1] || p[2]!=1)return;
 size_t header=9+(size_t)p[8];
 if(header>n)return;
 if((p[7]&0xc0)==0x80 || (p[7]&0xc0)==0xc0) {
  if(p[8]>=5) {
   const unsigned char *b=p+9;
   if((b[0]&1) && (b[2]&1) && (b[4]&1)) {
    a->pts=((uint64_t)(b[0]>>1&7)<<30)|((uint64_t)b[1]<<22)|
           ((uint64_t)(b[2]>>1)<<15)|((uint64_t)b[3]<<7)|(b[4]>>1);
    a->has_pts=1;
   }
  }
 }
 a->active=1;
 *data=p+header;*size=n-header;
}
void iptvita_pes_push(IptvitaPesAssembler *a,const unsigned char *ts,size_t size)
{
 if(!a || !a->data || !ts)return;
 for(size_t off=0;off+188<=size;off+=188) {
  const unsigned char *t=ts+off;
  if(t[0]!=0x47 || (((t[1]&31)<<8)|t[2])!=a->pid)continue;
  int adaptation=(t[3]>>4)&3;
  if(!(adaptation&1))continue;
  size_t pos=4;
  if(adaptation&2) {
   pos+=1+t[4];
   if(pos>188){a->active=0;a->used=0;a->dropped++;continue;}
  }
  if(pos>=188)continue;
  const unsigned char *payload=t+pos;
  size_t n=188-pos;
  if(t[1]&0x40) {
   emit(a);
   start(a,&payload,&n);
  }
  if(!a->active)continue;
  if(n>IPTVITA_PES_MAX-a->used) {
   a->active=0;a->used=0;a->dropped++;continue;
  }
  memcpy(a->data+a->used,payload,n);a->used+=n;
 }
}
void iptvita_pes_flush(IptvitaPesAssembler *a) {if(a)emit(a);}
void iptvita_pes_close(IptvitaPesAssembler *a)
{
 if(!a)return;
 free(a->data);a->data=NULL;a->used=0;a->active=0;
}
