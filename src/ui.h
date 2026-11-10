#ifndef IPTVITA_UI_H
#define IPTVITA_UI_H

#define IPTVITA_WIDTH 960
#define IPTVITA_HEIGHT 544
#define IPTVITA_NAV_COUNT 4
#define IPTVITA_SOURCE_COUNT 4
#include "sources.h"
#include "playlist.h"

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
    int language; /* 0 = pt-BR (default), 1 = English */
    int show_dialog;
    int quit_confirm;
    int list_mode;
    int browsing;
    int channel_index;
    int source_index;
    IptvitaPlaylist playlist;
    int list_index;
    int editing;
    int edit_index;
    int edit_field;
    int keyboard_index;
    int grid_keyboard;
    int keyboard_upper;
    int save_status;
    IptvitaSource draft;
    IptvitaSources sources;
} IptvitaUi;

void iptvita_ui_font_init(void);
void iptvita_ui_font_fini(void);
void iptvita_ui_render(const IptvitaUi *ui);
#endif
