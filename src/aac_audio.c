#include "aac_audio.h"
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>

#define FIFO 256
#define PREBUFFER 48
#define MAX_ADTS 8192
static const int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
typedef struct {unsigned short len;unsigned char bytes[MAX_ADTS];} Frame;
static Frame fifo[FIFO];
static unsigned head,tail,count;
static volatile int running,paused;
static int primed;
static SceUID mutex=-1,thread=-1;
static IptvitaAudioStats stats;
static SceAudiodecCtrl codec;
static SceAudiodecInfo info;
static int library_open,codec_open,port=-1,rate,ch,samples;
static unsigned char *es,*pcm;
static size_t es_cap;
static unsigned char carry[MAX_ADTS];
static size_t carry_len;

static void lock(void){if(mutex>=0)sceKernelLockMutex(mutex,1,NULL);}
static void unlock(void){if(mutex>=0)sceKernelUnlockMutex(mutex,1);}
static void close_audio(void)
{
 if(port>=0){sceAudioOutReleasePort(port);port=-1;}
 if(codec_open){sceAudiodecDeleteDecoder(&codec);codec_open=0;}
 free(es);free(pcm);es=pcm=NULL;es_cap=0;
 if(library_open){sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);library_open=0;}
 rate=ch=samples=0;
}
static int setup(int hz,int channels)
{
 if(codec_open && rate==hz && ch==channels)return 0;
 close_audio();
 if(channels<1 || channels>2 || (hz!=48000 && hz!=44100 && hz!=32000 && hz!=24000))return -41;
 SceAudiodecInitParam init;memset(&init,0,sizeof init);
 init.aac.size=sizeof(init.aac);
 init.aac.totalStreams=1;
 int rc=sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC,&init);
 if(rc<0 && (unsigned)rc!=0x807F0003u)return rc;
 library_open=(rc>=0);
 memset(&info,0,sizeof info);
 info.aac.size=sizeof(info.aac);
 info.aac.isAdts=1;
 info.aac.ch=(unsigned)channels;
 info.aac.samplingRate=(unsigned)hz;
 memset(&codec,0,sizeof codec);
 codec.size=sizeof codec;
 codec.wordLength=SCE_AUDIODEC_WORD_LENGTH_16BITS;
 codec.pInfo=&info;
 rc=sceAudiodecCreateDecoder(&codec,SCE_AUDIODEC_TYPE_AAC);
 if(rc<0)return rc;
 codec_open=1;
 unsigned e=codec.maxEsSize?codec.maxEsSize:SCE_AUDIODEC_AAC_MAX_ES_SIZE;
 unsigned p=codec.maxPcmSize?codec.maxPcmSize:SCE_AUDIODEC_AAC_MAX_SAMPLES*4;
 es=memalign(SCE_AUDIODEC_ALIGNMENT_SIZE,SCE_AUDIODEC_ROUND_UP(e));
 pcm=memalign(SCE_AUDIODEC_ALIGNMENT_SIZE,SCE_AUDIODEC_ROUND_UP(p));
 if(!es || !pcm)return -42;
 es_cap=e;
 rate=hz;ch=channels;
 return 0;
}
static int adts_rate(const unsigned char *d)
{
 int idx=(d[2]>>2)&15;
 return idx<13?rates[idx]:0;
}
static int adts_channels(const unsigned char *d)
{
 return ((d[2]&1)<<2)|(d[3]>>6);
}
static int worker(SceSize args,void *argp)
{
 (void)args;(void)argp;
 Frame *cur=(Frame*)malloc(sizeof(Frame));
 while(running) {
  int got=0;
  lock();
  if(!paused && count>=PREBUFFER)primed=1;
  if(primed && count && !paused && cur) {
   *cur=fifo[head];head=(head+1)%FIFO;count--;got=1;
  }
  if(!count)primed=0;
  unlock();
  if(!got){sceKernelDelayThread(5000);continue;}
  if(cur->len<7)continue;
  int hz=adts_rate(cur->bytes),channels=adts_channels(cur->bytes);
  int rc=setup(hz,channels);
  if(rc<0){stats.error=rc;continue;}
  if(cur->len>es_cap){stats.error=-43;continue;}
  memcpy(es,cur->bytes,cur->len);
  codec.pEs=es;codec.inputEsSize=cur->len;codec.pPcm=pcm;
  rc=sceAudiodecDecode(&codec);
  if(rc<0){stats.error=rc;continue;}
  int n=(int)(codec.outputPcmSize/(2u*(unsigned)ch));
  if(n<64 || n>2048 || n%64){stats.error=-44;continue;}
  if(port<0 || n!=samples) {
   if(port>=0)sceAudioOutReleasePort(port);
   port=sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,n,hz,
     ch==1?SCE_AUDIO_OUT_MODE_MONO:SCE_AUDIO_OUT_MODE_STEREO);
   if(port<0){stats.error=port;port=-1;continue;}
   samples=n;
  }
  rc=sceAudioOutOutput(port,pcm);
  if(rc<0)stats.error=rc;
  else {stats.played++;stats.rate=hz;stats.channels=ch;}
 }
 free(cur);
 close_audio();
 sceKernelExitDeleteThread(0);
 return 0;
}
int iptvita_audio_start(void)
{
 if(running)return 0;
 memset(&stats,0,sizeof stats);
 count=head=tail=0;carry_len=0;paused=0;primed=0;
 mutex=sceKernelCreateMutex("iptvita_audio_lock",0,0,NULL);
 if(mutex<0)return -1;
 running=1;
 thread=sceKernelCreateThread("iptvita_aac",worker,0x10000100,128*1024,0,0,NULL);
 if(thread<0){running=0;sceKernelDeleteMutex(mutex);mutex=-1;return -2;}
 if(sceKernelStartThread(thread,0,NULL)<0){running=0;sceKernelDeleteThread(thread);thread=-1;sceKernelDeleteMutex(mutex);mutex=-1;return -3;}
 return 0;
}
/* Accept audio PES containing ADTS frames; retain incomplete frame bytes
 * across PES boundaries. Unsupported LATM is not assumed to be ADTS. */
void iptvita_audio_push_pes(void *user,const unsigned char *data,size_t bytes,uint64_t pts,int has_pts)
{
 (void)user;(void)pts;(void)has_pts;
 if(!running || !data)return;
 for(size_t i=0;i<bytes;i++) {
  if(carry_len==MAX_ADTS){carry_len=0;stats.dropped++;}
  carry[carry_len++]=data[i];
  while(carry_len>=7) {
   if(carry[0]!=0xff || (carry[1]&0xf6)!=0xf0) {
    memmove(carry,carry+1,--carry_len);
    continue;
   }
   size_t n=((size_t)(carry[3]&3)<<11)|((size_t)carry[4]<<3)|(carry[5]>>5);
   if(n<7 || n>MAX_ADTS){memmove(carry,carry+1,--carry_len);continue;}
   if(carry_len<n)break;
   lock();
   if(count<FIFO) {
    Frame *f=&fifo[tail];
    f->len=(unsigned short)n;memcpy(f->bytes,carry,n);
    tail=(tail+1)%FIFO;count++;stats.queued++;
   }else stats.dropped++;
   unlock();
   carry_len-=n;memmove(carry,carry+n,carry_len);
  }
 }
}
void iptvita_audio_pause(int on){paused=on?1:0;}
IptvitaAudioStats iptvita_audio_stats(void){return stats;}
void iptvita_audio_stop(void)
{
 if(!running)return;
 running=0;
 if(thread>=0){sceKernelWaitThreadEnd(thread,NULL,NULL);thread=-1;}
 if(mutex>=0){sceKernelDeleteMutex(mutex);mutex=-1;}
 count=head=tail=0;carry_len=0;primed=0;
}
