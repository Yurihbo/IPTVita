#include "stream_diagnostic.h"
#include "network.h"
#include "hls.h"
#include "hls_crypto.h"
#include "video_pipeline.h"
#include "video_preview.h"
#include "aac_audio.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Alternate HLS audio uses its own TS demux; never feed audio-only PMTs
 * into the video transport, since that would erase the active video PID. */
static IptvitaTsPipeline alt_transport;
static IptvitaPesAssembler alt_pes;
static int alt_pid=-1;
static void reset_alt_audio(void)
{
 if(alt_pid>=0) {
  iptvita_pes_flush(&alt_pes);
  iptvita_pes_close(&alt_pes);
 }
 alt_pid=-1;
 iptvita_ts_init(&alt_transport);
}
static void feed_alt_audio(IptvitaUi *ui,const unsigned char *data,size_t size)
{
 if(!data || !size)return;
 if(size>=188 && data[0]==0x47 && size%188==0) {
  iptvita_ts_feed(&alt_transport,data,size);
  int pid=alt_transport.audio_type==0x0f?alt_transport.audio_pid:-1;
  ui->stream_alt_audio_type=alt_transport.audio_type;
  if(pid!=alt_pid) {
   if(alt_pid>=0) {
    iptvita_pes_flush(&alt_pes);
    iptvita_pes_close(&alt_pes);
   }
   alt_pid=-1;
   if(pid>=0 && iptvita_pes_open(&alt_pes,pid,iptvita_audio_push_pes,NULL)==0)
    alt_pid=pid;
  }
  if(alt_pid>=0) {
   iptvita_pes_push(&alt_pes,data,size);
   ui->stream_audio_pid=alt_pid;
   ui->stream_audio_type=0x0f;
  }
 }else if(size>=7 && data[0]==0xff && (data[1]&0xf6)==0xf0) {
  ui->stream_alt_audio_type=0x0f;
  ui->stream_audio_type=0x0f;
  iptvita_audio_push_pes(NULL,data,size,0,0);
 }
}
static IptvitaVideoPipeline pipeline;
static int pipeline_open;
static unsigned long long now_us(void) {return (unsigned long long)sceKernelGetProcessTimeWide();}
static void close_pipeline(void)
{
 reset_alt_audio();
 if(pipeline_open) {
  iptvita_video_close(&pipeline);
  pipeline_open=0;
 }
 iptvita_audio_stop();
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
 if((ui->stream_stage>=1 && ui->stream_stage<=3) || ui->stream_stage==5 || ui->stream_stage==6)return -2;
 close_pipeline();
 ui->stream_paused=0;ui->stream_media_url[0]=0;
 ui->stream_cipher_method=0;
 memset(ui->stream_key,0,sizeof(ui->stream_key));
 ui->stream_key_url[0]=0;ui->stream_segment_url[0]=0;
 ui->stream_key_valid=0;ui->stream_cached_key_url[0]=0;
 ui->stream_last_segment[0]=0;ui->stream_refresh_at=0;
 ui->stream_last_sequence=ui->stream_selected_sequence=0;
 ui->stream_has_sequence=0;
 ui->stream_packets=ui->stream_nals=ui->stream_frames=0;
 ui->stream_video_pid=-1;
 ui->stream_audio_pid=-1;ui->stream_audio_type=0;
 ui->stream_alt_audio_list_url[0]=0;
 ui->stream_alt_audio_segment_url[0]=0;
 ui->stream_alt_audio_key_url[0]=0;
 ui->stream_alt_audio_cached_key_url[0]=0;
 memset(ui->stream_alt_audio_key,0,16);
 ui->stream_alt_audio_key_valid=ui->stream_alt_audio_has_sequence=0;
 ui->stream_alt_audio_pending=0;ui->stream_alt_audio_cipher=0;
 ui->stream_alt_audio_last_sequence=0;
 ui->stream_alt_audio_selected_sequence=0;
 ui->stream_alt_audio_type=0;
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
 int alternate=ui->stream_stage==7;
 int rc=iptvita_hls_inspect_next(text,ui->stream_url,info,
       alternate?ui->stream_alt_audio_last_sequence:ui->stream_last_sequence,
       alternate?ui->stream_alt_audio_has_sequence:
          (ui->stream_has_sequence && ui->stream_stage==2));
 free(text);
 if(rc<0){free(info);fail(ui,-10);return;}
 if(info->cipher_method==2) {
  free(info);
  if(alternate) {
   ui->stream_alt_audio_list_url[0]=0;
   ui->stream_stage=5;
   ui->stream_refresh_at=now_us()+200000ULL;
  }else fail(ui,-23);
  return;
 }
 char next[IPTVITA_HLS_URL_MAX];
 snprintf(next,sizeof(next),"%s",info->first_media);
 int master=info->is_master;
 int cipher=info->cipher_method;
 if(master && info->audio_media[0]) {
  snprintf(ui->stream_alt_audio_list_url,sizeof(ui->stream_alt_audio_list_url),
           "%s",info->audio_media);
 }
 if(!master && alternate) {
  ui->stream_alt_audio_cipher=cipher;
  ui->stream_alt_audio_selected_sequence=info->media_sequence;
  memcpy(ui->stream_alt_audio_iv,info->iv,16);
  snprintf(ui->stream_alt_audio_key_url,sizeof(ui->stream_alt_audio_key_url),
           "%s",info->key_url);
 }else if(!master) {
  ui->stream_cipher_method=cipher;
  ui->stream_selected_sequence=info->media_sequence;
  memcpy(ui->stream_iv,info->iv,16);
  snprintf(ui->stream_key_url,sizeof(ui->stream_key_url),"%s",info->key_url);
 }
 free(info);
 if(!next[0]) {
  if(alternate) {
   ui->stream_stage=5;ui->stream_refresh_at=now_us()+350000ULL;
   return;
  }
  if(!master && ui->stream_has_sequence) {
   ui->stream_stage=5;ui->stream_refresh_at=now_us()+500000ULL;
   return;
  }
  fail(ui,-14);return;
 }
 int stage=master?2:(alternate?8:3);
 if((ui->stream_stage==2 || alternate) && master){fail(ui,-14);return;}
 if(!master && !alternate) {
  strncpy(ui->stream_media_url,ui->stream_url,sizeof(ui->stream_media_url)-1);
  ui->stream_media_url[sizeof(ui->stream_media_url)-1]=0;
  if(strcmp(next,ui->stream_last_segment)==0) {
   ui->stream_stage=5;
   ui->stream_refresh_at=now_us()+1500000ULL;
   return;
  }
 }
 if(alternate) {
  if(cipher==1) {
   if(!ui->stream_alt_audio_key_url[0]) {
    ui->stream_stage=5;
    ui->stream_alt_audio_list_url[0]=0;
    return;
   }
   snprintf(ui->stream_alt_audio_segment_url,sizeof(ui->stream_alt_audio_segment_url),"%s",next);
   if(ui->stream_alt_audio_key_valid &&
      strcmp(ui->stream_alt_audio_key_url,ui->stream_alt_audio_cached_key_url)==0)
     rc=schedule(ui,next,8);
   else rc=schedule(ui,ui->stream_alt_audio_key_url,9);
  }else rc=schedule(ui,next,8);
 }else if(!master && cipher==1) {
  if(!ui->stream_key_url[0]){fail(ui,-24);return;}
  snprintf(ui->stream_segment_url,sizeof(ui->stream_segment_url),"%s",next);
  if(ui->stream_key_valid &&
     strcmp(ui->stream_key_url,ui->stream_cached_key_url)==0)
   rc=schedule(ui,next,stage);
  else rc=schedule(ui,ui->stream_key_url,6);
 }else rc=schedule(ui,next,stage);
 if(rc<0)fail(ui,rc);
}
static void inspect_key(IptvitaUi *ui)
{
 SceUID fd=sceIoOpen(iptvita_network_cache_path(),SCE_O_RDONLY,0);
 if(fd<0){fail(ui,-25);return;}
 SceOff size=sceIoLseek(fd,0,SCE_SEEK_END);
 if(size!=16 || sceIoLseek(fd,0,SCE_SEEK_SET)<0) {
  sceIoClose(fd);fail(ui,-25);return;
 }
 unsigned char key[16];
 int n=sceIoRead(fd,key,16);
 sceIoClose(fd);
 if(n!=16){fail(ui,-25);return;}
 int alternate=ui->stream_stage==9;
 int rc;
 if(alternate) {
  memcpy(ui->stream_alt_audio_key,key,16);
  snprintf(ui->stream_alt_audio_cached_key_url,
    sizeof(ui->stream_alt_audio_cached_key_url),"%s",ui->stream_alt_audio_key_url);
  ui->stream_alt_audio_key_valid=1;
  memset(key,0,sizeof(key));
  rc=schedule(ui,ui->stream_alt_audio_segment_url,8);
 }else {
  memcpy(ui->stream_key,key,16);
  snprintf(ui->stream_cached_key_url,sizeof(ui->stream_cached_key_url),"%s",ui->stream_key_url);
  ui->stream_key_valid=1;
  memset(key,0,sizeof(key));
  rc=schedule(ui,ui->stream_segment_url,3);
 }
 if(rc<0)fail(ui,rc);
}
static void inspect_alt_audio(IptvitaUi *ui)
{
 SceUID fd=sceIoOpen(iptvita_network_cache_path(),SCE_O_RDONLY,0);
 if(fd<0){ui->stream_alt_audio_list_url[0]=0;ui->stream_stage=5;return;}
 SceOff length=sceIoLseek(fd,0,SCE_SEEK_END);
 if(length<=0 || length>6*1024*1024 ||
    sceIoLseek(fd,0,SCE_SEEK_SET)<0) {
  sceIoClose(fd);ui->stream_alt_audio_list_url[0]=0;ui->stream_stage=5;return;
 }
 unsigned char *buffer=(unsigned char*)malloc((size_t)length);
 if(!buffer) {
  sceIoClose(fd);ui->stream_stage=5;return;
 }
 int read=sceIoRead(fd,buffer,(unsigned int)length);
 sceIoClose(fd);
 int plain=read;
 if(read==(int)length && ui->stream_alt_audio_cipher==1) {
  if(!ui->stream_alt_audio_key_valid)plain=-1;
  else plain=iptvita_hls_decrypt(buffer,(size_t)read,ui->stream_alt_audio_key,
                                 ui->stream_alt_audio_iv);
 }
 if(plain>0)feed_alt_audio(ui,buffer,(size_t)plain);
 free(buffer);
 ui->stream_alt_audio_last_sequence=ui->stream_alt_audio_selected_sequence;
 ui->stream_alt_audio_has_sequence=1;
 ui->stream_stage=5;
 ui->stream_refresh_at=now_us()+100000ULL;
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
 int bytes=0;
 if(ui->stream_cipher_method==1) {
  SceOff size=sceIoLseek(fd,0,SCE_SEEK_END);
  if(size<=0 || size>6*1024*1024 || sceIoLseek(fd,0,SCE_SEEK_SET)<0) {
   fail(ui,-26);
  }else {
   unsigned char *segment=(unsigned char*)malloc((size_t)size);
   if(!segment){fail(ui,-26);}
   else {
    int n=sceIoRead(fd,segment,(unsigned int)size);
    if(n!=(int)size)fail(ui,-26);
    else {
     int plain=iptvita_hls_decrypt(segment,(size_t)n,ui->stream_key,ui->stream_iv);
     if(plain<=0 || plain%188)fail(ui,-27);
     else if(iptvita_video_feed(&pipeline,segment,(size_t)plain)<0)fail(ui,-17);
    }
    free(segment);
   }
  }
 }else {
  unsigned char chunk[188*64];
  while((bytes=sceIoRead(fd,chunk,sizeof(chunk)))>0) {
   if(bytes%188) {fail(ui,-16);break;}
   if(iptvita_video_feed(&pipeline,chunk,(size_t)bytes)<0){fail(ui,-17);break;}
  }
  if(bytes<0 && ui->stream_stage==3)fail(ui,-15);
 }
 /* Do not flush PES/AU at an HLS segment boundary. Audio and H264
  * packets can straddle adjacent transport segments; forced flush generated
  * truncated pictures and repeatedly restarted the sound. The next PES
  * start packet releases the previous completed payload naturally. */
 ui->stream_packets=pipeline.transport.packets;
 ui->stream_nals=pipeline.transport.h264_nal_units;
 ui->stream_video_pid=pipeline.transport.video_pid;
 ui->stream_audio_pid=pipeline.transport.audio_pid;
 ui->stream_audio_type=pipeline.transport.audio_type;
 IptvitaPreviewStats stats=iptvita_preview_stats();
 ui->stream_decode_error=stats.decode_error;
 ui->stream_decoded_frames=stats.decoded;
 ui->stream_video_width=stats.width;
 ui->stream_video_height=stats.height;
 sceIoClose(fd);
 if(ui->stream_stage==3) {
  strncpy(ui->stream_last_segment,ui->stream_url,sizeof(ui->stream_last_segment)-1);
  ui->stream_last_segment[sizeof(ui->stream_last_segment)-1]=0;
  ui->stream_has_sequence=1;
  ui->stream_last_sequence=ui->stream_selected_sequence;
  if(ui->stream_audio_pid<0 && ui->stream_alt_audio_list_url[0])
   ui->stream_alt_audio_pending=1;
  ui->stream_stage=5;
  ui->stream_refresh_at=now_us()+200000ULL;
 }
}
void iptvita_stream_poll(IptvitaUi *ui)
{
 if(!ui)return;
 if(ui->stream_stage==5) {
  IptvitaPreviewStats video=iptvita_preview_stats();
  if(video.fatal) {
   ui->stream_decode_error=video.decode_error;
   ui->stream_stage=-29;
   iptvita_audio_pause(1);
   return;
  }
  if(!ui->stream_paused && ui->stream_media_url[0] &&
     now_us()>=ui->stream_refresh_at) {
   int rc;
   if(ui->stream_alt_audio_pending && ui->stream_alt_audio_list_url[0]) {
    ui->stream_alt_audio_pending=0;
    rc=schedule(ui,ui->stream_alt_audio_list_url,7);
   }else rc=schedule(ui,ui->stream_media_url,2);
   if(rc<0)fail(ui,rc);
  }
  return;
 }
 if(ui->stream_stage<1 || (ui->stream_stage>3 &&
   ui->stream_stage!=6 && ui->stream_stage!=7 &&
   ui->stream_stage!=8 && ui->stream_stage!=9))return;
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
 else if(ui->stream_stage==8)inspect_alt_audio(ui);
 else if(ui->stream_stage==6 || ui->stream_stage==9)inspect_key(ui);
 else inspect_playlist(ui);
}

void iptvita_stream_stop(IptvitaUi *ui)
{
 close_pipeline();
 iptvita_audio_stop();
 iptvita_preview_reset();
 if(ui) {
  ui->stream_stage=0;
  ui->stream_paused=0;
  ui->stream_media_url[0]=0;
  ui->stream_last_segment[0]=0;
  ui->stream_key_valid=0;
  memset(ui->stream_key,0,sizeof(ui->stream_key));
  ui->stream_cached_key_url[0]=0;
  ui->stream_has_sequence=0;
  ui->stream_alt_audio_has_sequence=0;
  ui->stream_alt_audio_pending=0;
  ui->stream_alt_audio_list_url[0]=0;
  ui->stream_alt_audio_key_valid=0;
  memset(ui->stream_alt_audio_key,0,16);
 }
}
