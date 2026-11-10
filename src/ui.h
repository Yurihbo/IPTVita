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
    int channel_details;
    int player_probe_result;
    int player_probe_target;
    int stream_paused;
    int stream_fill_screen; /* L1 toggles contain vs center-cropped fill */
    unsigned long long stream_last_sequence,stream_selected_sequence;
    int stream_has_sequence;
    unsigned long long stream_refresh_at;
    char stream_media_url[8192];
    char stream_last_segment[8192];
    int stream_cipher_method;
    unsigned char stream_key[16],stream_iv[16];
    char stream_key_url[8192],stream_segment_url[8192];
    char stream_cached_key_url[8192];
    int stream_key_valid;
    int stream_info_visible; /* R1 toggles stream diagnostics; default on details view */
    int stream_stage; /* 0 idle, 1 master/media playlist, 2 media playlist, 3 TS segment, 5 waiting for playlist refresh, negative error */
    int stream_status;
    int stream_http;
    int stream_net_error;
    unsigned stream_packets,stream_nals,stream_frames;
    int stream_video_pid;
    int stream_decode_error;
    unsigned stream_decoded_frames;
    int stream_video_width,stream_video_height;
    char stream_url[8192];
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
