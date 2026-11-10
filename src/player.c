#include "player.h"
#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#include <string.h>

/* Native AVPlayer source acceptance probe. It does not render or play media.
 * In particular, HLS and HTTPS support cannot be assumed. */
int iptvita_player_probe(const char *url)
{
 if(!url || !url[0]) return -100;
 int module=sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
 if(module<0) return module;
 SceAvPlayerInitData init;
 memset(&init,0,sizeof(init));
 init.autoStart=0;
 init.numOutputVideoFrameBuffers=2;
 SceAvPlayerHandle handle=sceAvPlayerInit(&init);
 if(handle<0) return handle;
 int result=sceAvPlayerAddSource(handle,url);
 sceAvPlayerClose(handle);
 return result==0?1:result;
}
