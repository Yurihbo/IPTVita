#include "video_preview.h"
#include "avc_decoder.h"
#include "h264_sps.h"
#include <vita2d.h>
#include <stdlib.h>
#include <string.h>

static IptvitaAvcDecoder dec;
static int open;
static unsigned char *rgba;
static size_t rgba_size;
static vita2d_texture *texture;
static IptvitaPreviewStats st;

void iptvita_preview_reset(void)
{
 if(open)iptvita_avc_close(&dec);
 open=0;
 if(texture){vita2d_wait_rendering_done();vita2d_free_texture(texture);texture=NULL;}
 free(rgba);rgba=NULL;rgba_size=0;
 memset(&st,0,sizeof st);
}
void iptvita_preview_submit(const unsigned char *data,size_t size,uint64_t pts,int has_pts,int key)
{
 (void)key;
 if(!data || !size)return;
 if(!open && st.decode_error==0) {
  int w=0,h=0;
  if(iptvita_h264_dimensions(data,size,&w,&h)<0)return;
  if(w>1280 || h>720){st.decode_error=-30;return;}
  int rc=iptvita_avc_open(&dec,w,h,4);
  if(rc<0){st.decode_error=rc;return;}
  size_t stride=(size_t)((w+15)&~15);
  size_t lines=(size_t)((h+15)&~15);
  rgba_size=stride*lines*4u;
  rgba=(unsigned char*)malloc(rgba_size);
  texture=vita2d_create_empty_texture((unsigned)w,(unsigned)h);
  if(!rgba || !texture) {
   st.decode_error=-31;
   iptvita_avc_close(&dec);
   if(texture){vita2d_free_texture(texture);texture=NULL;}
   free(rgba);rgba=NULL;rgba_size=0;
   return;
  }
  st.width=w;st.height=h;
  open=1;
 }
 if(!open)return;
 st.submitted++;
 int rc=iptvita_avc_decode(&dec,data,size,pts,has_pts,rgba,rgba_size);
 if(rc<0){st.decode_error=rc;return;}
 if(rc==0)return;
 st.decoded++;
 /* Copy only visible pixels; Vita2D owns its own GPU texture stride. */
 unsigned char *dst=(unsigned char*)vita2d_texture_get_datap(texture);
 unsigned stride=vita2d_texture_get_stride(texture);
 unsigned src_stride=(unsigned)((st.width+15)&~15)*4u;
 if(!dst || stride<(unsigned)st.width*4u){st.decode_error=-32;return;}
 for(int y=0;y<st.height;y++)
  memcpy(dst+(size_t)y*stride,rgba+(size_t)y*src_stride,(size_t)st.width*4u);
}
IptvitaPreviewStats iptvita_preview_stats(void){return st;}
void iptvita_preview_draw(int x,int y,int width,int height)
{
 if(!texture || !st.decoded || !st.width || !st.height)return;
 float sx=(float)width/(float)st.width,sy=(float)height/(float)st.height;
 float scale=sx<sy?sx:sy;
 int dw=(int)(st.width*scale),dh=(int)(st.height*scale);
 vita2d_draw_texture_scale(texture,(float)(x+(width-dw)/2),(float)(y+(height-dh)/2),scale,scale);
}
