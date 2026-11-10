#include "ui.h"
#include "input.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
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

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    vita2d_init();

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
        quit = iptvita_handle_input(&ui, pressed);

        vita2d_start_drawing();
        iptvita_ui_render(&ui);
        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceKernelDelayThread(16000);
    }

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
