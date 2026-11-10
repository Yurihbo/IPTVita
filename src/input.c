#include "input.h"
#include "keyboard.h"
#include "network.h"
#include "player.h"
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
        if(pressed&SCE_CTRL_CIRCLE) {if(ui->channel_details) ui->channel_details=0;else ui->browsing=0;return 0;}
        if(ui->channel_details) {
            if((pressed&SCE_CTRL_TRIANGLE) && ui->playlist.count>0)
                {ui->player_probe_target=1;
                ui->player_probe_result=iptvita_player_probe(ui->playlist.channels[ui->channel_index].url);}
            if(pressed&SCE_CTRL_SQUARE) {
                ui->player_probe_target=2;
                ui->player_probe_result=iptvita_player_probe("ux0:data/IPTVita/test.mp4");
            }
            return 0;
        }
        if((pressed&SCE_CTRL_UP) && ui->channel_index>0) --ui->channel_index;
        if((pressed&SCE_CTRL_DOWN) && ui->channel_index+1<ui->playlist.count) ++ui->channel_index;
        if((pressed&SCE_CTRL_CROSS) && ui->playlist.count>0) {ui->channel_details=1;ui->player_probe_result=0;ui->player_probe_target=0;}
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
            ui->save_status=0;
            const IptvitaSource *source=&ui->sources.items[ui->source_index];
            const char *url=source->url;
            int remote=strncmp(url,"https://",8)==0 || strncmp(url,"http://",7)==0;
            if(strncmp(url,"ux0:",4)==0) {
                iptvita_playlist_load(&ui->playlist,url);
                ui->browsing=1;
            } else if(remote && source->type!=1) {
                /* Recognize online M3U by address, including older saved source types. */
                if(iptvita_network_start(url)==0) {
                    ui->browsing=1;
                    ui->playlist.count=0;
                    ui->playlist.status=0;
                } else ui->save_status=7;
            } else ui->save_status=6;
        }
        if((pressed&SCE_CTRL_RTRIGGER) && ui->sources.count)
            begin_edit(ui,ui->list_index,0);
        if((pressed&SCE_CTRL_SQUARE) && ui->sources.count) {
            iptvita_sources_remove(&ui->sources,(unsigned int)ui->list_index);
            ui->save_status=iptvita_sources_save(&ui->sources)<0?4:1;
            if(ui->list_index>=(int)ui->sources.count && ui->list_index>0) --ui->list_index;
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
            else if(ui->settings_index==2) ui->language=1-ui->language;
            else ui->show_dialog=1;
        } else if(ui->page==IPTVITA_HOME) open_page(ui,IPTVITA_SOURCES);
    }
    return 0;
}
