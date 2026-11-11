#include "avc_decoder.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>
#include <psp2/sysmodule.h>
#include <string.h>

#define AVC_TYPE SCE_VIDEODEC_TYPE_HW_AVCDEC
static unsigned align16(unsigned n) {return (n+15u)&~15u;}
void iptvita_avc_close(IptvitaAvcDecoder *d)
{
 if(!d)return;
 if(d->decoder_open)sceAvcdecDeleteDecoder(&d->ctrl);
 for(unsigned i=0;i<8;i++) {
  if(d->picture_mapped[i] && d->picture_data[i])
   sceGxmUnmapMemory(d->picture_data[i]);
  if(d->picture_uid[i]>=0)sceKernelFreeMemBlock(d->picture_uid[i]);
  d->picture_uid[i]=-1;
  d->picture_data[i]=NULL;
  d->picture_mapped[i]=0;
 }
 if(d->es_uid>=0)sceKernelFreeMemBlock(d->es_uid);
 d->es_uid=-1;d->es_data=NULL;d->es_capacity=0;
 d->picture_count=0;
 d->decoder_open=0;
 if(d->frame_uid>=0)sceKernelFreeMemBlock(d->frame_uid);
 d->frame_uid=-1;
 if(d->library_open)sceVideodecTermLibrary(AVC_TYPE);
 d->library_open=0;
 memset(&d->ctrl,0,sizeof(d->ctrl));
}
static int open_attempt(IptvitaAvcDecoder *d,int width,int height,int references)
{
 if(!d)return -1;
 memset(d,0,sizeof(*d));d->frame_uid=-1;d->es_uid=-1;
 for(unsigned i=0;i<8;i++)d->picture_uid[i]=-1;
 if(width<16 || height<16 || width>1280 || height>720)return -2;
 unsigned w=align16((unsigned)width),h=align16((unsigned)height);
 if(w*h/256u>3600)return -2;
 if(references<2)references=2;
 if(references>8)references=8;
 /* Match the public decoder's 18000-macroblock reference-picture budget.
  * Requesting more can poison its subsequent first-frame call with 0x80620002. */
 unsigned mbs=(w/16u)*(h/16u);
 int cap=mbs?(int)(18000u/mbs):2;
 if(cap<2)cap=2;
 if(references>cap)references=cap;
 /* AVCDEC lives in the Vita firmware; failure to load a separate sysmodule
  * is not necessarily fatal. The library call below is authoritative. */
 sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
 SceVideodecQueryInitInfoHwAvcdec init;
 memset(&init,0,sizeof(init));
 init.size=sizeof(init);
 init.horizontal=w;init.vertical=h;
 init.numOfRefFrames=(unsigned)references;
 init.numOfStreams=1;
 int ret=sceVideodecInitLibrary(AVC_TYPE,&init);
 if(ret<0)return ret;
 d->library_open=1;
 SceAvcdecQueryDecoderInfo query;
 SceAvcdecDecoderInfo memory;
 memset(&query,0,sizeof(query));
 memset(&memory,0,sizeof(memory));
 query.horizontal=w;query.vertical=h;query.numOfRefFrames=(unsigned)references;
 ret=sceAvcdecQueryDecoderMemSize(AVC_TYPE,&query,&memory);
 if(ret<0)goto error;
 /* Try both codec-visible memory pools. A CDRAM allocation can succeed
  * while AVCDEC still rejects it; physical main memory is a valid fallback. */
 unsigned needed=memory.frameMemSize;
 if(!needed || needed>64u*1024u*1024u){ret=-5;goto error;}
 ret=-6;
 for(int kind=0;kind<2;kind++) {
  unsigned align=kind?1024u*1024u:256u*1024u;
  unsigned alloc=(needed+align-1u)&~(align-1u);
  int type=kind?SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW:
                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
  SceUID uid=sceKernelAllocMemBlock("iptvita_avc_frame",type,alloc,NULL);
  if(uid<0){ret=uid;continue;}
  void *base=NULL;
  int got=sceKernelGetMemBlockBase(uid,&base);
  if(got<0 || !base){sceKernelFreeMemBlock(uid);ret=got<0?got:-6;continue;}
  memset(&d->ctrl,0,sizeof(d->ctrl));
  d->ctrl.frameBuf.pBuf=base;
  d->ctrl.frameBuf.size=alloc;
  got=sceAvcdecCreateDecoder(AVC_TYPE,&d->ctrl,&query);
  if(got>=0){d->frame_uid=uid;ret=0;break;}
  ret=got;
  sceKernelFreeMemBlock(uid);
  memset(&d->ctrl,0,sizeof(d->ctrl));
 }
 if(ret<0)goto error;
 d->decoder_open=1;d->width=width;d->height=height;
 /* The AVC hardware requires contiguous/codec-visible ES and output memory:
  * ordinary malloc buffers cannot be used for decoded picture slots. */
 unsigned es_alloc=2u*1024u*1024u;
 d->es_uid=sceKernelAllocMemBlock("iptvita_avc_es",SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,es_alloc,NULL);
 if(d->es_uid<0){ret=d->es_uid;goto error;}
 void *esbase=NULL;
 ret=sceKernelGetMemBlockBase(d->es_uid,&esbase);
 if(ret<0)goto error;
 d->es_data=(unsigned char*)esbase;d->es_capacity=es_alloc;
 /* The older Vita IPTV player starts with 3-4 hardware picture slots and
  * adds more only if memory is available. Requiring five or eight can itself
  * prevent playback. Map each slot for the GXM display/codec path. */
 unsigned pixels=w*h*4u;
 for(unsigned i=0;i<8;i++) {
  int mapped=0;
  for(int kind=0;kind<2 && !mapped;kind++) {
   unsigned align=kind?1024u*1024u:256u*1024u;
   unsigned picbytes=(pixels+align-1u)&~(align-1u);
   int type=kind?SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW:
                  SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW;
   SceUID uid=sceKernelAllocMemBlock("iptvita_avc_picture",type,picbytes,NULL);
   if(uid<0){ret=uid;continue;}
   void *picbase=NULL;
   int base_rc=sceKernelGetMemBlockBase(uid,&picbase);
   if(base_rc<0 || !picbase){
    sceKernelFreeMemBlock(uid);
    ret=base_rc<0?base_rc:-6;
    continue;
   }
   int map_rc=sceGxmMapMemory(picbase,picbytes,SCE_GXM_MEMORY_ATTRIB_READ|SCE_GXM_MEMORY_ATTRIB_WRITE);
   if(map_rc<0){
    sceKernelFreeMemBlock(uid);
    ret=map_rc;
    continue;
   }
   d->picture_uid[i]=uid;
   d->picture_data[i]=(unsigned char*)picbase;
   d->picture_mapped[i]=1;
   d->picture_count++;
   mapped=1;
  }
  if(!mapped) {
   if(d->picture_count<3)goto error;
   break;
  }
 }
 return 0;
error:
 iptvita_avc_close(d);
 return ret;
}

/* Some firmware/stream combinations cannot reserve enough reference
 * picture memory for the requested configuration. Try a lower count rather
 * than leaving an otherwise decodable stream permanently unavailable. */
int iptvita_avc_open(IptvitaAvcDecoder *d,int width,int height,int references)
{
 if(!d)return -1;
 if(references<2)references=2;
 if(references>8)references=8;
 unsigned aw=align16((unsigned)width),ah=align16((unsigned)height);
 if(width<16 || height<16 || width>1280 || height>720)return -2;
 unsigned mbs=(aw/16u)*(ah/16u);
 int cap=mbs?(int)(18000u/mbs):2;
 if(cap<2)cap=2;
 if(references>cap)references=cap;
 int rc=-1;
 for(int refs=references;refs>=2;refs--) {
  rc=open_attempt(d,width,height,refs);
  if(rc>=0)return 0;
 }
 return rc;
}

/* Submit a single complete Annex-B access unit using the documented public
 * AVC decoder. Callers own and retain the output buffer while decoding.
 * The decoder may legitimately buffer frames and produce no output. */
int iptvita_avc_decode(IptvitaAvcDecoder *d,const unsigned char *bytes,size_t size,
                       uint64_t pts,int has_pts,void *rgba,size_t rgba_capacity,
                       uint64_t *picture_pts,int *picture_has_pts)
{
 if(picture_has_pts)*picture_has_pts=0;
 if(!d || !d->decoder_open || !bytes || !size || size>0xffffffffu || !rgba)return -1;
 unsigned w=align16((unsigned)d->width),h=align16((unsigned)d->height);
 if(rgba_capacity<(size_t)w*h*4u)return -2;
 SceAvcdecAu access;
 SceAvcdecPicture picture[2],*pictures[2];
 SceAvcdecArrayPicture output;
 if(!d->es_data || size>d->es_capacity || d->picture_count<3)return -3;
 memcpy(d->es_data,bytes,size);
 memset(&access,0,sizeof(access));
 memset(picture,0,sizeof(picture));
 memset(&output,0,sizeof(output));
 access.es.pBuf=d->es_data;
 access.es.size=(unsigned)size;
 access.dts.upper=access.dts.lower=0xffffffffu;
 /* Preserve real MPEG PTS when supported by the firmware, enabling
  * picture.info.pts to identify delayed B-frame output unambiguously.
  * A few firmware/streams reject timestamps: retry once with sentinels. */
 if(has_pts && !d->disable_pts) {
  access.pts.upper=(unsigned)(pts>>32);
  access.pts.lower=(unsigned)pts;
 } else access.pts.upper=access.pts.lower=0xffffffffu;
 /* Public AVC API is designed for a small output picture array (up to 2);
  * passing all eight allocated slots in one call can exhaust codec state. */
 const unsigned outputs=1;
 for(unsigned i=0;i<outputs;i++) {
  unsigned slot=(d->slot+i)%d->picture_count;
  picture[i].size=sizeof(SceAvcdecPicture);
  picture[i].frame.pixelType=SCE_AVCDEC_PIXELFORMAT_RGBA8888;
  picture[i].frame.framePitch=w;
  picture[i].frame.frameWidth=w;
  picture[i].frame.frameHeight=h;
  picture[i].frame.opt.rgba.alpha=255;
  picture[i].frame.pPicture[0]=d->picture_data[slot];
  pictures[i]=&picture[i];
 }
 output.numOfElm=outputs;
 output.pPicture=pictures;
 int rc=sceAvcdecDecode(&d->ctrl,&access,&output);
 if(rc<0 && has_pts && !d->disable_pts &&
    ((unsigned)rc==0x80620002u || (unsigned)rc==0x80620802u)) {
  d->disable_pts=1;
  d->pts_retries++;
  access.pts.upper=access.pts.lower=0xffffffffu;
  output.numOfElm=outputs;output.numOfOutput=0;
  rc=sceAvcdecDecode(&d->ctrl,&access,&output);
 }
 /* The codec may retain an output slot even while returning zero pictures.
  * Rotate submitted slots on every AU, not just successful output. */
 d->slot=(d->slot+outputs)%d->picture_count;
 if(rc<0)return rc;
 if(!output.numOfOutput)return 0;
 if(output.numOfOutput>outputs)return -4;
 SceAvcdecPicture *shown=&picture[output.numOfOutput-1];
 if(picture_pts && picture_has_pts &&
    !(shown->info.pts.upper==0xffffffffu &&
      shown->info.pts.lower==0xffffffffu)) {
  *picture_pts=((uint64_t)shown->info.pts.upper<<32)|
               shown->info.pts.lower;
  *picture_has_pts=1;
 }
 memcpy(rgba,shown->frame.pPicture[0],(size_t)w*h*4u);
 return 1;
}
