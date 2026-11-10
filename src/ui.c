#include "ui.h"

#define BG       RGBA8(10, 23, 42, 255)
#define SURFACE  RGBA8(17, 39, 65, 255)
#define PANEL    RGBA8(24, 51, 81, 255)
#define SELECTED RGBA8(34, 81, 129, 255)
#define ACCENT   RGBA8(111, 192, 252, 255)
#define WHITE    RGBA8(245, 250, 255, 255)
#define MUTED    RGBA8(157, 181, 206, 255)

static void bars(int x, int y, int width, unsigned int color)
{
    vita2d_draw_rectangle(x, y, width, 3, color);
}

static void text_glyph(int x, int y, char ch, int scale, unsigned int color)
{
    /*
     * Original built-in 3x5 bitmap font. No external font or third-party
     * image assets are needed in Stage 1.
     */
    static const struct { char ch; unsigned short dots; } glyphs[] = {
        {'A',0x7bed}, {'B',0x6bad}, {'C',0x724e}, {'D',0x6b6b},
        {'E',0x72cf}, {'F',0x12cf}, {'G',0x7a4e}, {'H',0x5bed},
        {'I',0x7497}, {'J',0x3a92}, {'K',0x5bad}, {'L',0x7249},
        {'M',0x5f7d}, {'N',0x5f6d}, {'O',0x7b6f}, {'P',0x13ef},
        {'Q',0x7f6f}, {'R',0x5bef}, {'S',0x79ce}, {'T',0x2497},
        {'U',0x7b6d}, {'V',0x2b6d}, {'W',0x5f6d}, {'X',0x5aad},
        {'Y',0x2495}, {'Z',0x72a7}, {'0',0x7b6f}, {'1',0x7492},
        {'2',0x73e7}, {'3',0x79a7}, {'4',0x49ed}, {'5',0x79cf},
        {'6',0x7bcf}, {'7',0x2497}, {'8',0x7bef}, {'9',0x79ef},
        {'-',0x01c0}, {'.',0x4000}, {'/',0x0124}, {':',0x00a0},
        {' ',0x0000}
    };
    unsigned short pattern = 0;
    unsigned int i;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
        if (glyphs[i].ch == ch) {
            pattern = glyphs[i].dots;
            break;
        }
    }
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (pattern & (1u << (row * 3 + col))) {
                vita2d_draw_rectangle(x + col * scale, y + row * scale,
                                      scale - 1, scale - 1, color);
            }
        }
    }
}

static void label(int x, int y, const char *value, int scale, unsigned int color)
{
    while (*value) {
        text_glyph(x, y, *value, scale, color);
        x += 4 * scale;
        ++value;
    }
}

void iptvita_ui_render(const IptvitaUi *ui)
{
    static const char *items[IPTVITA_ITEM_COUNT] = {
        "M3U / M3U8", "XTREAM CODES", "DIRECT STREAM", "LOCAL M3U"
    };

    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, IPTVITA_WIDTH, IPTVITA_HEIGHT, BG);
    vita2d_draw_rectangle(0, 0, IPTVITA_WIDTH, 80, SURFACE);
    vita2d_draw_rectangle(0, 77, IPTVITA_WIDTH, 3, ACCENT);
    label(36, 24, "IPTVITA", 7, WHITE);
    label(36, 57, "YOUR STREAMS, YOUR WAY", 2, MUTED);

    label(44, 112, "ADD A SOURCE", 4, WHITE);
    for (int i = 0; i < IPTVITA_ITEM_COUNT; ++i) {
        int x = 44 + (i % 2) * 448;
        int y = 150 + (i / 2) * 126;
        int active = (ui->selected == i);
        vita2d_draw_rectangle(x, y, 420, 104, active ? SELECTED : PANEL);
        if (active) vita2d_draw_rectangle(x, y, 5, 104, ACCENT);
        label(x + 23, y + 25, items[i], 4, WHITE);
        label(x + 23, y + 65, "COMING SOON", 2, MUTED);
    }

    bars(44, 463, 868, PANEL);
    label(44, 487, "BY YURIHBO", 3, ACCENT);
    label(708, 489, "STAGE 1  V0.1", 2, MUTED);

    if (ui->show_about) {
        vita2d_draw_rectangle(240, 180, 480, 190, SURFACE);
        vita2d_draw_rectangle(240, 180, 480, 4, ACCENT);
        label(278, 213, "IPTVITA V0.1", 5, WHITE);
        label(278, 270, "FOUNDATION BUILD", 3, MUTED);
        label(278, 321, "CIRCLE TO CLOSE", 2, ACCENT);
    }
}
