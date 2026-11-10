#ifndef IPTVITA_UI_H
#define IPTVITA_UI_H

#define IPTVITA_WIDTH 960
#define IPTVITA_HEIGHT 544
#define IPTVITA_NAV_COUNT 4
#define IPTVITA_SOURCE_COUNT 4

typedef enum {
    IPTVITA_HOME = 0,
    IPTVITA_SOURCES = 1,
    IPTVITA_SETTINGS = 2,
    IPTVITA_ABOUT = 3
} IptvitaPage;

typedef struct {
    int page;
    int nav_index;
    int focus_content;
    int card_index;
    int settings_index;
    int theme;
    int show_dialog;
    int quit_confirm;
} IptvitaUi;

void iptvita_ui_render(const IptvitaUi *ui);
#endif
