#include "input.h"
#include <psp2/ctrl.h>

static void open_page(IptvitaUi *ui, int page)
{
    ui->page = page;
    ui->nav_index = page;
    ui->focus_content = page == IPTVITA_SOURCES || page == IPTVITA_SETTINGS;
    ui->show_dialog = 0;
}

int iptvita_handle_input(IptvitaUi *ui, unsigned int pressed)
{
    if (ui->quit_confirm) {
        if (pressed & SCE_CTRL_CROSS) return 1;
        if (pressed & SCE_CTRL_CIRCLE) ui->quit_confirm = 0;
        return 0;
    }

    if (ui->show_dialog) {
        if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) ui->show_dialog = 0;
        return 0;
    }

    if (pressed & SCE_CTRL_START) {
        ui->quit_confirm = 1;
        return 0;
    }

    if (pressed & SCE_CTRL_LTRIGGER) {
        open_page(ui, (ui->page + IPTVITA_NAV_COUNT - 1) % IPTVITA_NAV_COUNT);
        return 0;
    }
    if (pressed & SCE_CTRL_RTRIGGER) {
        open_page(ui, (ui->page + 1) % IPTVITA_NAV_COUNT);
        return 0;
    }

    if (pressed & SCE_CTRL_CIRCLE) {
        if (ui->page != IPTVITA_HOME || ui->focus_content) {
            open_page(ui, IPTVITA_HOME);
            ui->focus_content = 0;
        } else {
            ui->quit_confirm = 1;
        }
        return 0;
    }

    if (pressed & SCE_CTRL_LEFT) {
        if (ui->focus_content && ui->page == IPTVITA_SOURCES && (ui->card_index % 2) == 1) {
            --ui->card_index;
        } else {
            ui->focus_content = 0;
        }
    }
    if (pressed & SCE_CTRL_RIGHT) {
        if (!ui->focus_content) {
            ui->focus_content = 1;
        } else if (ui->page == IPTVITA_SOURCES && (ui->card_index % 2) == 0) {
            ++ui->card_index;
        }
    }

    if (pressed & SCE_CTRL_DOWN) {
        if (!ui->focus_content) {
            ui->nav_index = (ui->nav_index + 1) % IPTVITA_NAV_COUNT;
        } else if (ui->page == IPTVITA_SOURCES) {
            ui->card_index = (ui->card_index + 2) % IPTVITA_SOURCE_COUNT;
        } else if (ui->page == IPTVITA_SETTINGS) {
            ui->settings_index = (ui->settings_index + 1) % 3;
        }
    }
    if (pressed & SCE_CTRL_UP) {
        if (!ui->focus_content) {
            ui->nav_index = (ui->nav_index + IPTVITA_NAV_COUNT - 1) % IPTVITA_NAV_COUNT;
        } else if (ui->page == IPTVITA_SOURCES) {
            ui->card_index = (ui->card_index + 2) % IPTVITA_SOURCE_COUNT;
        } else if (ui->page == IPTVITA_SETTINGS) {
            ui->settings_index = (ui->settings_index + 2) % 3;
        }
    }

    if (pressed & SCE_CTRL_CROSS) {
        if (!ui->focus_content) {
            open_page(ui, ui->nav_index);
        } else if (ui->page == IPTVITA_SOURCES) {
            ui->show_dialog = 1; /* Stage 3 will implement input forms. */
        } else if (ui->page == IPTVITA_SETTINGS) {
            if (ui->settings_index == 0) {
                ui->theme = (ui->theme + 1) % 3;
            } else if (ui->settings_index == 2) {
                ui->language = 1 - ui->language;
            } else {
                ui->show_dialog = 1;
            }
        } else if (ui->page == IPTVITA_HOME) {
            open_page(ui, IPTVITA_SOURCES);
        }
    }
    return 0;
}
