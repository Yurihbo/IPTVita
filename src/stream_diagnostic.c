#include "stream_diagnostic.h"
#include "network.h"
#include "hls.h"
#include "video_pipeline.h"
#include "video_preview.h"
#include "aac_audio.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IptvitaVideoPipeline pipeline;
static int pipeline_open;
static unsigned long long now_us(void) {return (unsigned long long)sceKernelGetProcessTimeWide();}
static void close_pipeline(void)
{
 if(!pipeline_open)return;
 iptvita_video_close(&pipeline);
 iptvita_audio_stop();
 pipeline_open=0;
}
static void count_frame(void *user,const unsigned char *data,size_t size,
                        uint64_t pts,int has_pts,int keyframe)
{
 IptvitaUi *ui=(IptvitaUi*)user;
 ui->stream_frames++;
 iptvita_preview_submit(data,size,pts,has_pts,keyframe);
}
static void fail(IptvitaUi *ui,int error)
{
 ui->stream_status=error;
 ui->stream_stage=error;
}
static int schedule(IptvitaUi *ui,const char *url,int stage)
{
 size_t n=strlen(url);
 if(!n || n>=sizeof(ui->stream_url))return -14;
 memcpy(ui->stream_url,url,n+1);
 if(iptvita_network_start(url)<0)return -11;
 ui->stream_stage=stage;ui->stream_status=0;
 return 0;
}
int iptvita_stream_start(IptvitaUi *ui,const char *url)
{
 if(!ui || !url || !url[0])return -1;
 if((ui->stream_stage>=1 && ui->stream_stage<=3) || ui->stream_stage==5)return -2;
 close_pipeline();
 ui->stream_paused=0;ui->stream_media_url[0]=0;
 ui->stream_last_segment[0]=0;ui->stream_refresh_at=0;
 ui->stream_packets=ui->stream_nals=ui->stream_frames=0;
 ui->stream_video_pid=-1;
 ui->stream_decode_error=0;ui->stream_decoded_frames=0;
 ui->stream_video_width=ui->stream_video_height=0;
 iptvita_preview_reset();
 iptvita_audio_start();
 ui->stream_http=0;ui->stream_net_error=0;
 int ts=strstr(url,".ts")!=NULL && !strstr(url,".m3u8");
 int result=schedule(ui,url,ts?3:1);
 if(result<0)fail(ui,result);
 return result;
}
static void inspect_playlist(IptvitaUi *ui)
{
 SceUID fd=sceIoOpen(iptvita_network_cache_path(),SCE_O_RDONLY,0);
 if(fd<0){fail(ui,-12);return;}
 char *text=(char*)malloc(131073);
 if(!text){sceIoClose(fd);fail(ui,-12);return;}
 int n=sceIoRead(fd,text,131072);
 sceIoClose(fd);
 if(n<=0){free(text);fail(ui,-12);return;}
 text[n]=0;
 IptvitaHlsInfo *info=(IptvitaHlsInfo*)malloc(sizeof(*info));
 if(!info){free(text);fail(ui,-12);return;}
 int rc=iptvita_hls_inspect(text,ui->stream_url,info);
 free(text);
 if(rc<0){free(info);fail(ui,-10);return;}
 if(info->encrypted){free(info);fail(ui,-13);return;}
 char next[IPTVITA_HLS_URL_MAX];
 snprintf(next,sizeof(next),"%s",info->first_media);
 int master=info->is_master;
 free(info);
 if(!next[0]){fail(ui,-14);return;}
 int stage=master?2:3;
 if(ui->stream_stage==2 && master){fail(ui,-14);return;}
 if(!master) {
  strncpy(ui->stream_media_url,ui->stream_url,sizeof(ui->stream_media_url)-1);
  ui->stream_media_url[sizeof(ui->stream_media_url)-1]=0;
  if(strcmp(next,ui->stream_last_segment)==0) {
   ui->stream_stage=5;
   ui->stream_refresh_at=now_us()+1500000ULL;
   return;
  }
 }
 rc=schedule(ui,next,stage);
 if(rc<0)fail(ui,rc);
}
static void inspect_ts(IptvitaUi *ui)
{
 SceUID fd=sceIoOpen(iptvita_network_cache_path(),SCE_O_RDONLY,0);
 if(fd<0){fail(ui,-15);return;}
 if(!pipeline_open) {
  if(iptvita_video_open(&pipeline,count_frame,ui)<0) {
   sceIoClose(fd);fail(ui,-17);return;
  }
  pipeline_open=1;
 }
 unsigned char chunk[188*64];
 int bytes=0;
 while((bytes=sceIoRead(fd,chunk,sizeof(chunk)))>0) {
  if(bytes%188) {fail(ui,-16);break;}
  if(iptvita_video_feed(&pipeline,chunk,(size_t)bytes)<0){fail(ui,-17);break;}
 }
 if(bytes<0 && ui->stream_stage==3)fail(ui,-15);
 if(ui->stream_stage==3 && pipeline.video_pid>=0) {
  if(pipeline.audio_pid>=0)iptvita_pes_flush(&pipeline.audio_pes);
  iptvita_pes_flush(&pipeline.video_pes);
  iptvita_h264_au_flush(&pipeline.video_au);
 }
 ui->stream_packets=pipeline.transport.packets;
 ui->stream_nals=pipeline.transport.h264_nal_units;
 ui->stream_video_pid=pipeline.transport.video_pid;
 IptvitaPreviewStats stats=iptvita_preview_stats();
 ui->stream_decode_error=stats.decode_error;
 ui->stream_decoded_frames=stats.decoded;
 ui->stream_video_width=stats.width;
 ui->stream_video_height=stats.height;
 sceIoClose(fd);
 if(ui->stream_stage==3) {
  strncpy(ui->stream_last_segment,ui->stream_url,sizeof(ui->stream_last_segment)-1);
  ui->stream_last_segment[sizeof(ui->stream_last_segment)-1]=0;
  ui->stream_stage=5;
  ui->stream_refresh_at=now_us()+1500000ULL;
 }
}
void iptvita_stream_poll(IptvitaUi *ui)
{
 if(!ui)return;
 if(ui->stream_stage==5) {
  if(!ui->stream_paused && ui->stream_media_url[0] &&
     now_us()>=ui->stream_refresh_at) {
   int rc=schedule(ui,ui->stream_media_url,2);
   if(rc<0)fail(ui,rc);
  }
  return;
 }
 if(ui->stream_stage<1 || ui->stream_stage>3)return;
 int net=iptvita_network_state();
 if(net==1 || net==0)return;
 int err=iptvita_network_error_code();
 ui->stream_http=iptvita_network_http_status();
 ui->stream_net_error=err;
 /* Reset the finished request BEFORE scheduling another one. Resetting
  * afterwards could erase a newly completed worker's status. */
 iptvita_network_reset();
 if(net!=2) {
  fail(ui,err?err:(ui->stream_http ? -20 : -11));
  return;
 }
 if(ui->stream_stage==3)inspect_ts(ui);
 else inspect_playlist(ui);
}
