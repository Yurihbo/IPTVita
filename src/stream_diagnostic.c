#include "stream_diagnostic.h"
#include "network.h"
#include "hls.h"
#include "video_pipeline.h"
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void count_frame(void *user,const unsigned char *data,size_t size,
                        uint64_t pts,int has_pts,int keyframe)
{
 IptvitaUi *ui=(IptvitaUi*)user;
 (void)data;(void)size;(void)pts;(void)has_pts;(void)keyframe;
 ui->stream_frames++;
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
 if(ui->stream_stage>=1 && ui->stream_stage<=3)return -2;
 ui->stream_packets=ui->stream_nals=ui->stream_frames=0;
 ui->stream_video_pid=-1;
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
 /* Stop variant recursion; playlist -> master -> media segment only. */
 if(ui->stream_stage==2 && master){fail(ui,-14);return;}
 rc=schedule(ui,next,stage);
 if(rc<0)fail(ui,rc);
}
static void inspect_ts(IptvitaUi *ui)
{
 SceUID fd=sceIoOpen(iptvita_network_cache_path(),SCE_O_RDONLY,0);
 if(fd<0){fail(ui,-15);return;}
 IptvitaVideoPipeline *v=(IptvitaVideoPipeline*)malloc(sizeof(*v));
 if(!v){sceIoClose(fd);fail(ui,-17);return;}
 if(iptvita_video_open(v,count_frame,ui)<0){free(v);sceIoClose(fd);fail(ui,-17);return;}
 unsigned char chunk[188*64];
 int bytes=0;
 while((bytes=sceIoRead(fd,chunk,sizeof(chunk)))>0) {
  if(bytes%188) {fail(ui,-16);break;}
  if(iptvita_video_feed(v,chunk,(size_t)bytes)<0){fail(ui,-17);break;}
 }
 if(bytes<0 && ui->stream_stage==3)fail(ui,-15);
 ui->stream_packets=v->transport.packets;
 ui->stream_nals=v->transport.h264_nal_units;
 ui->stream_video_pid=v->transport.video_pid;
 iptvita_video_close(v);
 free(v);
 sceIoClose(fd);
 if(ui->stream_stage==3)ui->stream_stage=ui->stream_packets?4:-16;
}
void iptvita_stream_poll(IptvitaUi *ui)
{
 if(!ui || ui->stream_stage<1 || ui->stream_stage>3)return;
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
