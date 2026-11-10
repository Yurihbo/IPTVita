#include "input.h"

int iptvita_handle_input(IptvitaUi *ui, unsigned int pressed)
{
    if (pressed & PSP2_CTRL_DOWN) {
        ui->selected = (ui->selected + 1) % IPTVITA_ITEM_COUNT;
    }
    if (pressed & PSP2_CTRL_UP) {
        ui->selected = (ui->selected + IPTVITA_ITEM_COUNT - 1) % IPTVITA_ITEM_COUNT;
    }
    if (pressed & PSP2_CTRL_CROSS) {
        ui->show_about = 1;
    }
    if (pressed & PSP2_CTRL_CIRCLE) {
        if (ui->show_about) {
            ui->show_about = 0;
        } else {
            return 1;
        }
    }
    return 0;
}
