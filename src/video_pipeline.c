#include "video_pipeline.h"
#include "aac_audio.h"
#include <string.h>

int iptvita_video_open(IptvitaVideoPipeline *v,IptvitaAuSink sink,void *user)
{
 if(!v || !sink)return -1;
 memset(v,0,sizeof(*v));
 iptvita_ts_init(&v->transport);
 v->video_pid=-1;v->audio_pid=-1;
 v->sink=sink;v->user=user;
 if(iptvita_h264_au_open(&v->video_au,sink,user)<0)return -2;
 v->active=1;return 0;
}
int iptvita_video_feed(IptvitaVideoPipeline *v,const unsigned char *ts,size_t size)
{
 if(!v || !v->active || !ts || size%188)return -1;
 /* Discovery before demux; the first packets containing the PMT are
  * inspected and ignored by PES, as they never carry AVC payload. */
 int n=iptvita_ts_feed(&v->transport,ts,size);
 int discovered=v->transport.video_pid;
 if(discovered!=v->video_pid) {
  if(v->video_pid>=0) {
   iptvita_pes_flush(&v->video_pes);
   iptvita_pes_close(&v->video_pes);
   iptvita_h264_au_flush(&v->video_au);
  }
  v->video_pid=-1;
  if(discovered>=0) {
   if(iptvita_pes_open(&v->video_pes,discovered,iptvita_h264_au_pes,&v->video_au)<0)return -2;
   v->video_pid=discovered;
  }
 }
 if(v->video_pid>=0)iptvita_pes_push(&v->video_pes,ts,size);
 int apid=v->transport.audio_pid;
 /* Rebind or release audio when PMT changes (common across ad insertion).
  * Do not keep pushing stale PID bytes after an audio format change. */
 int playable=v->transport.audio_type==0x0f ? apid : -1;
 if(playable!=v->audio_pid) {
  if(v->audio_pid>=0) {
   iptvita_pes_flush(&v->audio_pes);
   iptvita_pes_close(&v->audio_pes);
  }
  v->audio_pid=-1;
  if(playable>=0 &&
     iptvita_pes_open(&v->audio_pes,playable,iptvita_audio_push_pes,NULL)==0)
   v->audio_pid=playable;
 }
 if(v->audio_pid>=0)iptvita_pes_push(&v->audio_pes,ts,size);
 return n;
}
void iptvita_video_close(IptvitaVideoPipeline *v)
{
 if(!v || !v->active)return;
 if(v->audio_pid>=0){iptvita_pes_flush(&v->audio_pes);iptvita_pes_close(&v->audio_pes);}
 if(v->video_pid>=0) {
  iptvita_pes_flush(&v->video_pes);
  iptvita_pes_close(&v->video_pes);
 }
 iptvita_h264_au_flush(&v->video_au);
 iptvita_h264_au_close(&v->video_au);
 v->active=0;
}
