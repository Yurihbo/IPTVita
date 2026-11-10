#include "h264_sps.h"
#include <stdint.h>
#include <string.h>

typedef struct { unsigned char bytes[512]; size_t bits,pos; int error; } Bits;
static unsigned take(Bits *b,unsigned n)
{
 unsigned v=0;
 for(unsigned i=0;i<n;i++) {
  if(b->pos>=b->bits){b->error=1;return 0;}
  v=(v<<1)|((b->bytes[b->pos>>3]>>(7-(b->pos&7)))&1);
  b->pos++;
 }
 return v;
}
static unsigned ue(Bits *b)
{
 unsigned z=0;
 while(!b->error && !take(b,1)) {
  if(++z>23){b->error=1;return 0;}
 }
 if(b->error)return 0;
 return ((1u<<z)-1u)+take(b,z);
}
static int se(Bits *b){unsigned u=ue(b);return (u&1)?(int)((u+1)/2):-(int)(u/2);}
static void scaling(Bits *b,int size)
{
 int last=8,next=8;
 for(int i=0;i<size && !b->error;i++) {
  if(next)next=(last+se(b)+256)%256;
  last=next?next:last;
 }
}
static int parse(const unsigned char *src,size_t len,int *width,int *height)
{
 Bits b;memset(&b,0,sizeof b);
 if(len<4 || (src[0]&31)!=7)return -1;
 size_t used=0;
 for(size_t i=1;i<len && used<sizeof(b.bytes);i++) {
  if(i>=3 && src[i]==3 && src[i-1]==0 && src[i-2]==0)continue;
  b.bytes[used++]=src[i];
 }
 if(used<3)return -1;
 b.bits=used*8;
 unsigned profile=take(&b,8);
 take(&b,8);take(&b,8);ue(&b);
 unsigned chroma=1,separate=0;
 if(profile==100 || profile==110 || profile==122 || profile==244 ||
    profile==44 || profile==83 || profile==86 || profile==118 ||
    profile==128 || profile==138 || profile==139 || profile==134 || profile==135) {
  chroma=ue(&b);
  if(chroma>3)return -1;
  if(chroma==3)separate=take(&b,1);
  ue(&b);ue(&b);take(&b,1);
  if(take(&b,1))for(unsigned i=0;i<(chroma!=3?8u:12u);i++)if(take(&b,1))scaling(&b,i<6?16:64);
 }
 ue(&b);unsigned poc=ue(&b);
 if(poc==0)ue(&b);
 else if(poc==1) {
  take(&b,1);se(&b);se(&b);
  unsigned n=ue(&b);
  if(n>255)return -1;
  for(unsigned i=0;i<n;i++)se(&b);
 }
 ue(&b);take(&b,1);
 unsigned mbw=ue(&b)+1,mbh=ue(&b)+1;
 unsigned frame_only=take(&b,1);
 if(!frame_only)take(&b,1);
 take(&b,1);
 unsigned crop_left=0,crop_right=0,crop_top=0,crop_bottom=0;
 if(take(&b,1)) {
  crop_left=ue(&b);crop_right=ue(&b);crop_top=ue(&b);crop_bottom=ue(&b);
 }
 if(b.error || mbw>512 || mbh>512)return -1;
 unsigned subw=chroma==1 || chroma==2?2:1;
 unsigned subh=chroma==1?2:1;
 if(chroma==0 || separate){subw=1;subh=1;}
 unsigned unitx=subw,unity=subh*(2-frame_only);
 unsigned w=mbw*16,h=mbh*16*(2-frame_only);
 unsigned cx=(crop_left+crop_right)*unitx,cy=(crop_top+crop_bottom)*unity;
 if(cx>=w || cy>=h)return -1;
 w-=cx;h-=cy;
 if(w<16 || h<16 || w>4096 || h>4096)return -1;
 *width=(int)w;*height=(int)h;
 return 0;
}
int iptvita_h264_dimensions(const unsigned char *data,size_t size,int *width,int *height)
{
 if(!data || !width || !height)return -1;
 for(size_t i=0;i+5<size;i++) {
  size_t sc=0;
  if(data[i]==0 && data[i+1]==0 && data[i+2]==1)sc=3;
  else if(i+4<size && data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1)sc=4;
  if(!sc)continue;
  size_t start=i+sc;
  if((data[start]&31)!=7)continue;
  size_t end=start+1;
  for(;end+3<size;end++)
   if(data[end]==0 && data[end+1]==0 && (data[end+2]==1 || (end+3<size && data[end+2]==0 && data[end+3]==1)))break;
  return parse(data+start,end-start,width,height);
 }
 return -1;
}
