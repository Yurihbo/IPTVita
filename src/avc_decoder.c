#include "avc_decoder.h"
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <string.h>

#define AVC_TYPE SCE_VIDEODEC_TYPE_HW_AVCDEC
static unsigned align16(unsigned n) {return (n+15u)&~15u;}
void iptvita_avc_close(IptvitaAvcDecoder *d)
{
 if(!d)return;
 if(d->decoder_open)sceAvcdecDeleteDecoder(&d->ctrl);
 d->decoder_open=0;
 if(d->frame_uid>=0)sceKernelFreeMemBlock(d->frame_uid);
 d->frame_uid=-1;
 if(d->library_open)sceVideodecTermLibrary(AVC_TYPE);
 d->library_open=0;
 memset(&d->ctrl,0,sizeof(d->ctrl));
}
int iptvita_avc_open(IptvitaAvcDecoder *d,int width,int height,int references)
{
 if(!d)return -1;
 memset(d,0,sizeof(*d));d->frame_uid=-1;
 if(width<16 || height<16 || width>1280 || height>720)return -2;
 unsigned w=align16((unsigned)width),h=align16((unsigned)height);
 if(w*h/256u>3600)return -2;
 if(references<2)references=2;
 if(references>4)references=4;
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
 SceKernelAllocMemBlockOpt opt;
 memset(&opt,0,sizeof(opt));
 opt.size=sizeof(opt);
 opt.attr=SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
 opt.alignment=256*1024;
 unsigned alloc=(memory.frameMemSize+262143u)&~262143u;
 d->frame_uid=sceKernelAllocMemBlock("iptvita_avc_frame",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,alloc,&opt);
 if(d->frame_uid<0){ret=d->frame_uid;goto error;}
 void *base=0;
 ret=sceKernelGetMemBlockBase(d->frame_uid,&base);
 if(ret<0)goto error;
 d->ctrl.frameBuf.pBuf=base;
 d->ctrl.frameBuf.size=alloc;
 ret=sceAvcdecCreateDecoder(AVC_TYPE,&d->ctrl,&query);
 if(ret<0)goto error;
 d->decoder_open=1;d->width=width;d->height=height;
 return 0;
error:
 iptvita_avc_close(d);
 return ret;
}

/* Submit a single complete Annex-B access unit using the documented public
 * AVC decoder. Callers own and retain the output buffer while decoding.
 * The decoder may legitimately buffer frames and produce no output. */
int iptvita_avc_decode(IptvitaAvcDecoder *d,const unsigned char *bytes,size_t size,
                       uint64_t pts,int has_pts,void *rgba,size_t rgba_capacity)
{
 if(!d || !d->decoder_open || !bytes || !size || size>0xffffffffu || !rgba)return -1;
 unsigned w=align16((unsigned)d->width),h=align16((unsigned)d->height);
 if(rgba_capacity<(size_t)w*h*4u)return -2;
 SceAvcdecAu access;
 SceAvcdecPicture picture,*pictures[1];
 SceAvcdecArrayPicture output;
 memset(&access,0,sizeof(access));
 memset(&picture,0,sizeof(picture));
 memset(&output,0,sizeof(output));
 access.es.pBuf=(void*)bytes;
 access.es.size=(unsigned)size;
 access.dts.upper=access.dts.lower=0xffffffffu;
 access.pts.upper=has_pts?(unsigned)(pts>>32):0xffffffffu;
 access.pts.lower=has_pts?(unsigned)pts:0xffffffffu;
 picture.size=sizeof(picture);
 picture.frame.pixelType=SCE_AVCDEC_PIXELFORMAT_RGBA8888;
 picture.frame.framePitch=w;
 picture.frame.frameWidth=w;
 picture.frame.frameHeight=h;
 picture.frame.opt.rgba.alpha=255;
 picture.frame.pPicture[0]=rgba;
 pictures[0]=&picture;
 output.numOfElm=1;
 output.pPicture=pictures;
 int rc=sceAvcdecDecode(&d->ctrl,&access,&output);
 if(rc<0)return rc;
 return output.numOfOutput?1:0;
}
