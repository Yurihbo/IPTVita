#include "ui.h"
#include "input.h"
#include <psp2/ctrl.h>
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

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    vita2d_init();

    while (!quit) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int pressed = pad.buttons & ~previous;
        previous = pad.buttons;
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
