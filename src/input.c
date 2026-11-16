#include "input.h"
#include "keyboard.h"
#include "network.h"
#include "player.h"
#include "stream_diagnostic.h"
#include "aac_audio.h"
#include "video_preview.h"
#include "favorites.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <string.h>
#include <stdio.h>

/* Controller-operated keyboard; no heavyweight IME or external assets. */
static const char keys[] = "abcdefghijklmnopqrstuvwxyz0123456789:/._-?&=%+@#";
#define KEY_COUNT ((int)(sizeof(keys)-1))
#define KEY_COLS 11

static void open_page(IptvitaUi *ui, int page)
{
    ui->page=page;
    ui->nav_index=page;
    ui->focus_content=(page==IPTVITA_SOURCES || page==IPTVITA_SETTINGS);
    ui->show_dialog=0;
    ui->delete_confirm=0;
    ui->browsing=0;
}
static char *field(IptvitaUi *ui, size_t *cap)
{
    if (ui->edit_field==0) { *cap=sizeof(ui->draft.name); return ui->draft.name; }
    if (ui->edit_field==1) { *cap=sizeof(ui->draft.url); return ui->draft.url; }
    if (ui->edit_field==2) { *cap=sizeof(ui->draft.username); return ui->draft.username; }
    *cap=sizeof(ui->draft.password); return ui->draft.password;
}
static void begin_edit(IptvitaUi *ui, int index, int type)
{
    memset(&ui->draft,0,sizeof(ui->draft));
    ui->edit_index=index;
    if(index>=0) ui->draft=ui->sources.items[index];
    else {
        ui->draft.type=(unsigned int)type;
        snprintf(ui->draft.name,sizeof(ui->draft.name),"FONTE %u",ui->sources.count+1);
    }
    ui->editing=1;
    ui->edit_field=0;
    ui->keyboard_index=0;
    ui->grid_keyboard=0;
    ui->keyboard_upper=0;
    ui->save_status=0;
}
static void finish_edit(IptvitaUi *ui)
{
    if(!ui->draft.name[0] || !ui->draft.url[0]) { ui->save_status=2; return; }
    /* M3U providers frequently give users a host/path without https://.
     * Normalize the source ADDRESS only; never rewrite channel URLs or
     * touch the decoder, audio clocks, or playback configuration. */
    if(ui->draft.type==0 &&
       strncmp(ui->draft.url,"http://",7)!=0 &&
       strncmp(ui->draft.url,"https://",8)!=0 &&
       strncmp(ui->draft.url,"ux0:",4)!=0 &&
       !strstr(ui->draft.url,"://")) {
        size_t n=strlen(ui->draft.url);
        if(n+8>=sizeof(ui->draft.url)) {ui->save_status=7;return;}
        memmove(ui->draft.url+8,ui->draft.url,n+1);
        memcpy(ui->draft.url,"https://",8);
    }
    if(ui->edit_index>=0) {
        ui->sources.items[ui->edit_index]=ui->draft;
    } else if(iptvita_sources_add(&ui->sources,&ui->draft)<0) {
        ui->save_status=3; return;
    }
    ui->save_status=iptvita_sources_save(&ui->sources)<0 ? 4 : 1;
    ui->editing=0;
    ui->list_mode=1;
    ui->list_index=(int)ui->sources.count-1;
    if(ui->edit_index>=0) ui->list_index=ui->edit_index;
}
/* Selection is always an index into the ORIGINAL M3U list. The compact
 * favorites view is only a navigation filter, never another stream source. */
static int favorite_position(const IptvitaUi *ui,int channel)
{
    for(int i=0;i<ui->favorite_count;i++)
        if(ui->favorite_channel_indices[i]==channel)return i;
    return -1;
}
static int next_channel(const IptvitaUi *ui,int direction)
{
    if(ui->playlist.count<=0 || ui->playlist.status!=1)return -1;
    if(ui->favorites_only) {
        if(ui->favorite_count<=0)return -1;
        int pos=favorite_position(ui,ui->channel_index);
        if(pos<0)return ui->favorite_channel_indices[
            direction>=0?0:ui->favorite_count-1];
        int next=(pos+direction+ui->favorite_count)%ui->favorite_count;
        return ui->favorite_channel_indices[next];
    }
    return (ui->channel_index+direction+ui->playlist.count)%ui->playlist.count;
}
static void tune_channel(IptvitaUi *ui,int channel)
{
    if(ui->playlist.status!=1 || channel<0 ||
       channel>=ui->playlist.count)return;
    const char *url=ui->playlist.channels[channel].url;
    if(!url || !url[0])return;
    /* The active network worker, audio thread and decoder must finish
     * BEFORE opening a new channel. Never start two HLS streams at once. */
    if(ui->channel_details)iptvita_stream_stop(ui);
    ui->channel_index=channel;
    ui->channel_details=1;
    ui->favorite_notice=0; /* Don't repeat a previous favorite toast on zap. */
    ui->player_probe_result=0;
    ui->player_probe_target=0;
    ui->stream_stage=0;
    ui->stream_quality_sd=0;
    ui->stream_hd_fallback_used=0;
    snprintf(ui->stream_source_url,sizeof(ui->stream_source_url),"%s",url);
    ui->channel_hint_until_us=
      (unsigned long long)sceKernelGetProcessTimeWide()+3500000ULL;
    iptvita_stream_start(ui,ui->stream_source_url);
}
static void toggle_channel_favorite(IptvitaUi *ui)
{
    if(ui->source_index<0 || ui->source_index>=(int)ui->sources.count ||
       ui->playlist.status!=1 || ui->channel_index<0 ||
       ui->channel_index>=ui->playlist.count)return;
    const char *url=ui->playlist.channels[ui->channel_index].url;
    const char *source=ui->sources.items[ui->source_index].url;
    if(!url || !url[0])return;
    int result=iptvita_favorites_toggle(source,url);
    ui->favorite_notice=result<0?-1:result?1:2;
    if(result>=0)iptvita_favorites_rebuild(ui);
    ui->channel_hint_until_us=
      (unsigned long long)sceKernelGetProcessTimeWide()+3500000ULL;
}
static void select_favorites_view(IptvitaUi *ui)
{
    ui->favorites_only=!ui->favorites_only;
    ui->favorite_notice=0;
    if(ui->favorites_only && ui->favorite_count>0 &&
       favorite_position(ui,ui->channel_index)<0)
        ui->channel_index=ui->favorite_channel_indices[0];
}
static void editor_input(IptvitaUi *ui,unsigned int pressed)
{
    if(pressed&SCE_CTRL_CIRCLE) { ui->editing=0; ui->save_status=0; return; }
    if(pressed&SCE_CTRL_SELECT) { ui->grid_keyboard=!ui->grid_keyboard; return; }
    if(pressed&SCE_CTRL_START) { ui->keyboard_upper=!ui->keyboard_upper; return; }
    if(pressed&SCE_CTRL_TRIANGLE) { finish_edit(ui); return; }
    int fields=ui->draft.type==1 ? 4 : 2;
    if(pressed&SCE_CTRL_LTRIGGER) ui->edit_field=(ui->edit_field+fields-1)%fields;
    if(pressed&SCE_CTRL_RTRIGGER) ui->edit_field=(ui->edit_field+1)%fields;
    if(ui->grid_keyboard && (pressed&SCE_CTRL_LEFT)) ui->keyboard_index=(ui->keyboard_index+KEY_COUNT-1)%KEY_COUNT;
    if(ui->grid_keyboard && (pressed&SCE_CTRL_RIGHT)) ui->keyboard_index=(ui->keyboard_index+1)%KEY_COUNT;
    if(ui->grid_keyboard && (pressed&SCE_CTRL_UP)) ui->keyboard_index=(ui->keyboard_index+KEY_COUNT-KEY_COLS)%KEY_COUNT;
    if(ui->grid_keyboard && (pressed&SCE_CTRL_DOWN)) ui->keyboard_index=(ui->keyboard_index+KEY_COLS)%KEY_COUNT;
    size_t cap=0;
    char *dst=field(ui,&cap);
    size_t n=strlen(dst);
    if((pressed&SCE_CTRL_SQUARE) && n) dst[n-1]=0;
    if(pressed&SCE_CTRL_CROSS) {
        if(ui->grid_keyboard) {
            if(n+1<cap) { char key=keys[ui->keyboard_index]; if(ui->keyboard_upper && key>='a' && key<='z') key=(char)(key-'a'+'A'); dst[n]=key; dst[n+1]=0; }
        } else {
            int result=iptvita_keyboard_begin(dst,cap,ui->edit_field==1,ui->edit_field==3,ui->language);
            if(result==-1) ui->grid_keyboard=1; /* IME unavailable or text too long */
        }
    }
    ui->save_status=0;
}
int iptvita_handle_input(IptvitaUi *ui,unsigned int pressed)
{
    if(ui->editing) { editor_input(ui,pressed); return 0; }
    if(ui->delete_confirm) {
        /* No destructive action until explicit YES followed by Cross.
         * Circle always cancels, including after the YES option is selected. */
        if(pressed&SCE_CTRL_CIRCLE){ui->delete_confirm=0;return 0;}
        if(pressed&(SCE_CTRL_LEFT|SCE_CTRL_RIGHT|SCE_CTRL_UP|SCE_CTRL_DOWN))
            ui->delete_yes=!ui->delete_yes;
        if(pressed&SCE_CTRL_CROSS) {
            if(ui->delete_yes && ui->delete_index>=0 &&
               ui->delete_index<(int)ui->sources.count) {
                iptvita_sources_remove(&ui->sources,(unsigned)ui->delete_index);
                ui->save_status=iptvita_sources_save(&ui->sources)<0?4:1;
                if(ui->list_index>=(int)ui->sources.count && ui->list_index>0)
                    --ui->list_index;
            }
            ui->delete_confirm=0;
        }
        return 0;
    }
    if(ui->quit_confirm) {
        if(pressed&SCE_CTRL_CROSS) return 1;
        if(pressed&SCE_CTRL_CIRCLE) ui->quit_confirm=0;
        return 0;
    }
    if(ui->show_dialog) {
        if(pressed&(SCE_CTRL_CROSS|SCE_CTRL_CIRCLE)) ui->show_dialog=0;
        return 0;
    }
    if(ui->browsing) {
        if(pressed&SCE_CTRL_CIRCLE) {
            if(ui->channel_details) {
                iptvita_stream_stop(ui);
                ui->channel_details=0;
            }else ui->browsing=0;
            return 0;
        }
        if(ui->channel_details) {
            /* Zapping is strictly menu/input plumbing. The underlying
             * player is reopened unchanged through its existing API. */
            if(pressed&SCE_CTRL_LEFT) {
                int next=next_channel(ui,-1);
                if(next>=0 && next!=ui->channel_index)tune_channel(ui,next);
                return 0;
            }
            if(pressed&SCE_CTRL_RIGHT) {
                int next=next_channel(ui,1);
                if(next>=0 && next!=ui->channel_index)tune_channel(ui,next);
                return 0;
            }
            if(pressed&SCE_CTRL_START) {
                toggle_channel_favorite(ui);
                return 0;
            }
            if(pressed&SCE_CTRL_SELECT) {
                iptvita_stream_stop(ui);
                ui->channel_details=0;
                ui->favorites_only=1;
                if(ui->favorite_count>0 &&
                   favorite_position(ui,ui->channel_index)<0)
                    ui->channel_index=ui->favorite_channel_indices[0];
                return 0;
            }
            if((pressed&SCE_CTRL_CROSS) && ui->stream_stage>=1 && ui->stream_stage<=6) {
                ui->stream_paused=!ui->stream_paused;
                iptvita_audio_pause(ui->stream_paused);
                iptvita_preview_pause(ui->stream_paused);
            }
            if(pressed&SCE_CTRL_RTRIGGER) ui->stream_info_visible=!ui->stream_info_visible;
            if(pressed&SCE_CTRL_LTRIGGER) ui->stream_fill_screen=!ui->stream_fill_screen;
            if((pressed&SCE_CTRL_TRIANGLE) && ui->playlist.count>0) {
                int current=ui->channel_index;
                tune_channel(ui,current);
            }
            if(pressed&SCE_CTRL_SQUARE) {
                ui->stream_stage=0;
                ui->stream_status=0;
                ui->player_probe_target=2;
                ui->player_probe_result=iptvita_player_probe("ux0:data/IPTVita/test.mp4");
            }
            return 0;
        }
        if(pressed&SCE_CTRL_SELECT) {select_favorites_view(ui);return 0;}
        if(pressed&SCE_CTRL_START) {
            if(!ui->favorites_only || ui->favorite_count)
                toggle_channel_favorite(ui);
            if(ui->favorites_only && ui->favorite_count &&
               favorite_position(ui,ui->channel_index)<0)
                ui->channel_index=ui->favorite_channel_indices[0];
            return 0;
        }
        if(pressed&SCE_CTRL_UP) {
            int next=next_channel(ui,-1);
            if(next>=0)ui->channel_index=next;
        }
        if(pressed&SCE_CTRL_DOWN) {
            int next=next_channel(ui,1);
            if(next>=0)ui->channel_index=next;
        }
        if(pressed&SCE_CTRL_CROSS) {
            if(!ui->favorites_only || ui->favorite_count) {
                ui->stream_info_visible=0;
                ui->stream_fill_screen=1;
                tune_channel(ui,ui->channel_index);
            }
        }
        return 0;
    }
    if(pressed&SCE_CTRL_START) { ui->quit_confirm=1; return 0; }
    if(ui->page==IPTVITA_SOURCES && (pressed&SCE_CTRL_SELECT)) {
        ui->list_mode=!ui->list_mode;
        ui->focus_content=1;
        ui->save_status=0;
        return 0;
    }
    if(ui->page==IPTVITA_SOURCES && ui->list_mode && ui->focus_content) {
        if(pressed&SCE_CTRL_UP && ui->list_index>0) --ui->list_index;
        if(pressed&SCE_CTRL_DOWN && ui->list_index+1<(int)ui->sources.count) ++ui->list_index;
        if((pressed&SCE_CTRL_CROSS) && ui->sources.count) {
            ui->source_index=ui->list_index;
            ui->channel_index=0;
            ui->channel_details=0;
            ui->favorites_only=0;
            ui->favorite_count=0;
            ui->favorite_notice=0;
            ui->save_status=0;
            const IptvitaSource *source=&ui->sources.items[ui->source_index];
            const char *url=source->url;
            if(strncmp(url,"ux0:",4)==0) {
                iptvita_playlist_load(&ui->playlist,url);
                iptvita_favorites_rebuild(ui);
                ui->browsing=1;
            } else if(source->type!=1) {
                /* Also support older saved M3U links without a scheme. */
                char full_url[IPTVITA_URL_MAX];
                const char *remote=url;
                if(strncmp(url,"https://",8)!=0 &&
                   strncmp(url,"http://",7)!=0 && !strstr(url,"://")) {
                    if(snprintf(full_url,sizeof(full_url),"https://%s",url)>=
                       (int)sizeof(full_url)) {
                        ui->save_status=7;
                        return 0;
                    }
                    remote=full_url;
                }
                if(iptvita_network_start_playlist(remote)==0) {
                    iptvita_playlist_clear(&ui->playlist);
                    ui->browsing=1;
                    ui->playlist.status=0;
                } else ui->save_status=7;
            } else ui->save_status=6;
        }
        if((pressed&SCE_CTRL_RTRIGGER) && ui->sources.count)
            begin_edit(ui,ui->list_index,0);
        if((pressed&SCE_CTRL_SQUARE) && ui->sources.count) {
            ui->delete_index=ui->list_index;
            ui->delete_yes=0;
            ui->delete_confirm=1;
        }
        if(pressed&SCE_CTRL_CIRCLE) ui->list_mode=0;
        return 0;
    }
    if(pressed&SCE_CTRL_LTRIGGER) { open_page(ui,(ui->page+IPTVITA_NAV_COUNT-1)%IPTVITA_NAV_COUNT); return 0; }
    if(pressed&SCE_CTRL_RTRIGGER) { open_page(ui,(ui->page+1)%IPTVITA_NAV_COUNT); return 0; }
    if(pressed&SCE_CTRL_CIRCLE) {
        if(ui->page!=IPTVITA_HOME || ui->focus_content) {
            open_page(ui,IPTVITA_HOME);
            ui->focus_content=0;
        } else ui->quit_confirm=1;
        return 0;
    }
    if(pressed&SCE_CTRL_LEFT) {
        if(ui->focus_content && ui->page==IPTVITA_SOURCES && (ui->card_index%2)==1) --ui->card_index;
        else ui->focus_content=0;
    }
    if(pressed&SCE_CTRL_RIGHT) {
        if(!ui->focus_content) ui->focus_content=1;
        else if(ui->page==IPTVITA_SOURCES && (ui->card_index%2)==0) ++ui->card_index;
    }
    if(pressed&SCE_CTRL_DOWN) {
        if(!ui->focus_content) ui->nav_index=(ui->nav_index+1)%IPTVITA_NAV_COUNT;
        else if(ui->page==IPTVITA_SOURCES) ui->card_index=(ui->card_index+2)%IPTVITA_SOURCE_COUNT;
        else if(ui->page==IPTVITA_SETTINGS) ui->settings_index=(ui->settings_index+1)%3;
    }
    if(pressed&SCE_CTRL_UP) {
        if(!ui->focus_content) ui->nav_index=(ui->nav_index+IPTVITA_NAV_COUNT-1)%IPTVITA_NAV_COUNT;
        else if(ui->page==IPTVITA_SOURCES) ui->card_index=(ui->card_index+2)%IPTVITA_SOURCE_COUNT;
        else if(ui->page==IPTVITA_SETTINGS) ui->settings_index=(ui->settings_index+2)%3;
    }
    if(pressed&SCE_CTRL_CROSS) {
        if(!ui->focus_content) open_page(ui,ui->nav_index);
        else if(ui->page==IPTVITA_SOURCES) {
            if(ui->sources.count<IPTVITA_MAX_SOURCES) begin_edit(ui,-1,ui->card_index);
            else ui->save_status=3;
        } else if(ui->page==IPTVITA_SETTINGS) {
            if(ui->settings_index==0) ui->theme=(ui->theme+1)%3;
            else if(ui->settings_index==1) ui->hide_control_hints=!ui->hide_control_hints;
            else ui->language=1-ui->language;
        } else if(ui->page==IPTVITA_HOME) open_page(ui,IPTVITA_SOURCES);
    }
    return 0;
}
