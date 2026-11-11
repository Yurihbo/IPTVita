#include "ui.h"
#include "input.h"
#include "keyboard.h"
#include "network.h"
#include "stream_diagnostic.h"
#include "video_preview.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
/* User activity while watching video should keep the display awake. */
#include <psp2/kernel/threadmgr.h>
#include <vita2d.h>

int main(void)
{
    static IptvitaUi ui = {0};
    iptvita_sources_load(&ui.sources);
    SceCtrlData pad = {0};
    unsigned int previous = 0;
    int quit = 0;
    unsigned int hold_frames = 0;
    unsigned int last_direction = 0;
    int touch_was_down = 0;
    unsigned long long last_power_tick = 0;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    vita2d_init();
    iptvita_ui_font_init();

    while (!quit) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int direction = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if(ui.editing) {
            if(pad.lx < 60) direction |= SCE_CTRL_LEFT;
            if(pad.lx > 195) direction |= SCE_CTRL_RIGHT;
            if(pad.ly < 60) direction |= SCE_CTRL_UP;
            if(pad.ly > 195) direction |= SCE_CTRL_DOWN;
        }
        unsigned int pressed = pad.buttons & ~previous;
        if(direction != last_direction) hold_frames=0;
        else if(direction) ++hold_frames;
        if(direction && (hold_frames==0 || (hold_frames>15 && hold_frames%3==0)))
            pressed |= direction;
        last_direction=direction;
        previous=pad.buttons;
        if(ui.editing) {
            SceTouchData touch={0};
            sceTouchPeek(SCE_TOUCH_PORT_FRONT,&touch,1);
            if(touch.reportNum && !touch_was_down) {
                int x=(int)touch.report[0].x*960/1920;
                int y=(int)touch.report[0].y*544/1088;
                if(x>=263 && x<912 && y>=273 && y<453) {
                    int col=(x-263)/59, row=(y-273)/36;
                    int index=row*11+col;
                    if(index>=0 && index<48) {
                        ui.keyboard_index=index;
                        ui.grid_keyboard=1;
                        pressed |= SCE_CTRL_CROSS;
                    }
                }
            }
            touch_was_down=touch.reportNum!=0;
        } else touch_was_down=0;
        if(iptvita_keyboard_active()) {
            iptvita_keyboard_tick();
            previous=pad.buttons;
            touch_was_down=0;
        } else {
            quit = iptvita_handle_input(&ui, pressed);
        }

        if(ui.browsing && ui.playlist.status==0) {
            int network=iptvita_network_state();
            if(network==2) {
                iptvita_playlist_load(&ui.playlist,iptvita_network_cache_path());
                iptvita_network_reset();
            } else if(network<0) {
                ui.playlist.status=network==-3?-5:network==-2?-6:-7;
                iptvita_network_reset();
            }
        }
        if(ui.browsing && ui.channel_details) {
            /* Vita's screen dim / auto-suspend timers normally treat video
             * playback as idle. Refresh them only while a player is open.
             * Do not override the physical power button or system suspend. */
            unsigned long long now=(unsigned long long)sceKernelGetProcessTimeWide();
            if(!last_power_tick || now-last_power_tick>=15000000ULL) {
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                last_power_tick=now;
            }
            iptvita_stream_poll(&ui);
        }else last_power_tick=0;
        if(ui.browsing && ui.channel_details) {
            if(!ui.stream_paused) iptvita_preview_tick();
            IptvitaPreviewStats stats=iptvita_preview_stats();
            ui.stream_decode_error=stats.decode_error;
            ui.stream_decoded_frames=stats.decoded;
            ui.stream_video_width=stats.width;
            ui.stream_video_height=stats.height;
        }
        vita2d_start_drawing();
        iptvita_ui_render(&ui);
        vita2d_end_drawing();
        vita2d_swap_buffers();
        /* vita2d_swap_buffers may already synchronize with VBlank. Adding
         * another 16 ms here halves the number of chances to feed AVCDEC on
         * 30 fps streams. Yield briefly without throttling the player. */
        sceKernelDelayThread(1000);
    }

    iptvita_stream_stop(&ui);
    iptvita_ui_font_fini();
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
