#ifndef IPTVITA_UI_H
#define IPTVITA_UI_H

#include <vita2d.h>

#define IPTVITA_WIDTH 960
#define IPTVITA_HEIGHT 544
#define IPTVITA_ITEM_COUNT 4

typedef struct {
    int selected;
    int show_about;
} IptvitaUi;

void iptvita_ui_render(const IptvitaUi *ui);
#endif
