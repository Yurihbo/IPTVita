#include "ui.h"
#include "network.h"
#include "video_preview.h"
#include "network.h"
#include "aac_audio.h"
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define C(r,g,b) RGBA8(r,g,b,255)

/*
 * IPTVita stage 2: an original 5x5 pixel font and lightweight UI.
 * The interface uses only filled rectangles, with no texture allocations.
 */
typedef struct {
    char ch;
    unsigned char rows[5];
} Glyph;
static const Glyph font[] = {
    {'A', {0x0E,0x11,0x1F,0x11,0x11}},
    {'B', {0x1E,0x11,0x1E,0x11,0x1E}},
    {'C', {0x0F,0x10,0x10,0x10,0x0F}},
    {'D', {0x1E,0x11,0x11,0x11,0x1E}},
    {'E', {0x1F,0x10,0x1E,0x10,0x1F}},
    {'F', {0x1F,0x10,0x1E,0x10,0x10}},
    {'G', {0x0F,0x10,0x17,0x11,0x0F}},
    {'H', {0x11,0x11,0x1F,0x11,0x11}},
    {'I', {0x1F,0x04,0x04,0x04,0x1F}},
    {'J', {0x07,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x16,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x1E,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x15,0x0F}},
    {'R', {0x1E,0x11,0x1E,0x12,0x11}},
    {'S', {0x0F,0x10,0x0E,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04}},
    {'Z', {0x1F,0x02,0x04,0x08,0x1F}},
    {'0', {0x0E,0x13,0x15,0x15,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x02,0x04,0x1F}},
    {'3', {0x1E,0x01,0x06,0x01,0x1E}},
    {'4', {0x12,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x1E}},
    {'6', {0x07,0x10,0x1E,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08}},
    {'8', {0x0E,0x11,0x0E,0x11,0x0E}},
    {'9', {0x0E,0x11,0x0F,0x01,0x0E}},
    {'/', {0x01,0x02,0x04,0x08,0x10}},
    {'-', {0x00,0x00,0x1F,0x00,0x00}},
    {'.', {0x00,0x00,0x00,0x00,0x04}},
    {':', {0x00,0x04,0x00,0x04,0x00}},
    {'+', {0x00,0x04,0x1F,0x04,0x00}},
    {'(', {0x02,0x04,0x08,0x08,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x08}},
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'?', {0x0E,0x11,0x02,0x04,0x04}},
    {'!', {0x04,0x04,0x04,0x00,0x04}},
    {',', {0x00,0x00,0x00,0x04,0x08}},
    {'=', {0x00,0x1F,0x00,0x1F,0x00}},
    {'&', {0x0C,0x12,0x0C,0x12,0x0D}},
    {'%', {0x19,0x1A,0x04,0x0B,0x13}},
    {'@', {0x0E,0x11,0x17,0x10,0x0E}},
    {'#', {0x0A,0x1F,0x0A,0x1F,0x0A}},
    {'_', {0x00,0x00,0x00,0x00,0x1F}},
};

typedef struct {
    unsigned int bg, sidebar, surface, raised, selected, accent, white, muted;
} Palette;

static Palette palette(int theme)
{
    if (theme == 1) {
        Palette p = {C(8,20,33), C(13,32,50), C(21,44,65), C(28,57,80),
                     C(39,88,118), C(95,205,220), C(244,250,255), C(153,181,204)};
        return p;
    }
    if (theme == 2) {
        Palette p = {C(9,15,33), C(18,25,51), C(29,38,70), C(39,51,84),
                     C(57,73,129), C(148,170,254), C(244,249,255), C(159,173,208)};
        return p;
    }
    Palette p = {C(9,22,40), C(13,31,54), C(20,43,71), C(29,55,87),
                 C(37,78,123), C(112,193,251), C(244,249,255), C(155,182,208)};
    return p;
}

static void rect(int x, int y, int w, int h, unsigned int color)
{
    vita2d_draw_rectangle(x, y, w, h, color);
}
static vita2d_pgf *system_font;
void iptvita_ui_font_init(void) { system_font=vita2d_load_default_pgf(); }
void iptvita_ui_font_fini(void) { if(system_font) { vita2d_free_pgf(system_font); system_font=NULL; } }
static void label(int x, int y, const char *s, int scale, unsigned int color)
{
    if(system_font) {
        float size=scale==1?0.55f:scale==2?0.75f:scale==3?1.00f:scale==4?1.30f:scale==5?1.55f:2.05f;
        vita2d_pgf_draw_text(system_font,x,y+(int)(15.0f*size),color,size,s);
        return;
    }
    while (*s) {
        char ch = *s++;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        const unsigned char *rows = NULL;
        for (size_t i = 0; i < sizeof(font)/sizeof(font[0]); ++i) {
            if (font[i].ch == ch) { rows = font[i].rows; break; }
        }
        if (rows) {
            for (int r = 0; r < 5; ++r) {
                unsigned char mask = rows[r];
                for (int c = 0; c < 5; ++c)
                    if (mask & (1u << (4 - c)))
                        rect(x + c*scale, y + r*scale, scale > 1 ? scale-1 : 1, scale > 1 ? scale-1 : 1, color);
            }
        }
        x += 6*scale;
    }
}
static void stroke(int x, int y, int w, int h, unsigned int color)
{
    rect(x,y,w,2,color); rect(x,y+h-2,w,2,color);
    rect(x,y,2,h,color); rect(x+w-2,y,2,h,color);
}
static void badge(int x, int y, const char *value, unsigned int fg, unsigned int bg)
{
    rect(x,y,180,27,bg);
    label(x+12,y+8,value,2,fg);
}
/* Draw recognizable PlayStation button symbols instead of ASCII commands.
 * Purely decorative: controller mappings remain in input.c. */
enum { HINT_CROSS, HINT_CIRCLE, HINT_SQUARE, HINT_TRIANGLE,
       HINT_LR, HINT_R, HINT_START, HINT_SELECT };
static void hint_button(int x,int y,int kind,const char *action,Palette p)
{
    int w=kind==HINT_LR?39:(kind==HINT_START||kind==HINT_SELECT?54:23);
    rect(x,y,w,24,p.raised);
    stroke(x,y,w,24,p.muted);
    unsigned int color=p.accent;
    if(kind==HINT_CROSS) {
        vita2d_draw_line(x+6,y+6,x+17,y+18,color);
        vita2d_draw_line(x+17,y+6,x+6,y+18,color);
    }else if(kind==HINT_CIRCLE) {
        const int dx[]={0,4,8,11,8,4,0,-3,0};
        const int dy[]={-8,-7,-4,0,4,7,8,0,-8};
        for(int i=0;i<8;i++)
            vita2d_draw_line(x+11+dx[i],y+12+dy[i],
                             x+11+dx[i+1],y+12+dy[i+1],color);
    }else if(kind==HINT_SQUARE) {
        stroke(x+6,y+6,12,12,color);
    }else if(kind==HINT_TRIANGLE) {
        vita2d_draw_line(x+12,y+5,x+5,y+19,color);
        vita2d_draw_line(x+5,y+19,x+19,y+19,color);
        vita2d_draw_line(x+19,y+19,x+12,y+5,color);
    }else {
        label(x+5,y+6,kind==HINT_LR?"L/R":
                          kind==HINT_R?"R":
                          kind==HINT_START?"START":"SELECT",1,color);
    }
    label(x+w+7,y+4,action,2,p.muted);
}
static const char *tr(const IptvitaUi *ui, const char *pt, const char *en)
{
    return ui->language ? en : pt;
}

static void sidebar(const IptvitaUi *ui, Palette p)
{
    static const char *pages_pt[]={"INICIO","FONTES","AJUSTES","SOBRE"};
    static const char *pages_en[]={"HOME","SOURCES","SETTINGS","ABOUT"};
    const char **pages = ui->language ? pages_en : pages_pt;
    rect(0,0,222,544,p.sidebar);
    rect(0,0,222,4,p.accent);
    label(23,31,"IPTVITA",4,p.white);
    label(24,70,tr(ui,"SUA CENTRAL IPTV","YOUR MEDIA HUB"),2,p.muted);
    rect(22,101,176,2,p.raised);
    for (int i=0;i<IPTVITA_NAV_COUNT;++i) {
        int y=136+i*70;
        int active=ui->page==i;
        rect(14,y,194,52,active?p.selected:p.sidebar);
        if(active) rect(14,y,4,52,p.accent);
        if(!ui->focus_content && ui->nav_index==i) stroke(14,y,194,52,p.accent);
        label(36,y+19,pages[i],2,active?p.white:p.muted);
    }
    rect(22,452,176,2,p.raised);
    label(23,476,"BY YURIHBO",2,p.accent);
    label(24,511,"IPTVITA  V0.6.0",2,p.muted);
}
static void heading(int page, Palette p, const char *title, const char *hint)
{
    (void)page;
    label(252,31,title,4,p.white);
    label(254,73,hint,2,p.muted);
    rect(251,106,677,2,p.surface);
}
static void home(const IptvitaUi *ui, Palette p)
{
    heading(ui->page,p,tr(ui,"BEM-VINDO","WELCOME"),tr(ui,"UM IPTV LEVE PARA PS VITA","A LIGHTWEIGHT IPTV PLAYER FOR PS VITA"));
     rect(252,130,672,148,p.surface);
    rect(252,130,5,148,p.accent);
    label(278,157,tr(ui,"SUAS FONTES","YOUR STREAMS"),4,p.white);
    label(280,203,tr(ui,"TODAS AS SUAS FONTES EM UM LUGAR","ONE PLACE FOR ALL YOUR SOURCES"),2,p.muted);
    label(280,241,tr(ui,"ABRA FONTES PARA CONTINUAR","OPEN SOURCES TO CONTINUE"),2,p.accent);
    rect(252,299,324,146,p.raised);
    rect(595,299,329,146,p.raised);
    char total[16]; snprintf(total,sizeof(total),"%u",ui->sources.count);
    label(277,327,total,7,p.white);
    label(278,395,tr(ui,"FONTES SALVAS","SAVED SOURCES"),2,p.muted);
    label(617,338,"HLS  AES-128",3,p.white);
    label(617,387,tr(ui,"DISPONIVEL PARA USO","READY TO USE"),2,p.accent);
    if(!ui->hide_control_hints) {
        hint_button(252,487,HINT_LR,tr(ui,"TELAS","PAGES"),p);
        hint_button(435,487,HINT_CROSS,tr(ui,"SELECIONAR","SELECT"),p);
        hint_button(657,487,HINT_START,tr(ui,"SAIR","EXIT"),p);
    }
}
static void clipped(int x,int y,const char *value,int maxchars,int scale,unsigned int color)
{
    char visible[80];
    int n=0;
    while(value[n] && n<maxchars && n<(int)sizeof(visible)-1) {
        visible[n]=value[n]; ++n;
    }
    visible[n]=0;
    label(x,y,visible,scale,color);
}
static void editor(const IptvitaUi *ui, Palette p)
{
    static const char *fields_pt[]={"NOME","ENDERECO URL","USUARIO","SENHA"};
    static const char *fields_en[]={"NAME","URL ADDRESS","USERNAME","PASSWORD"};
    static const char keyboard[]="abcdefghijklmnopqrstuvwxyz0123456789:/._-?&=%+@#";
    const char **names=ui->language?fields_en:fields_pt;
    const char *value=ui->edit_field==0?ui->draft.name:
                      ui->edit_field==1?ui->draft.url:
                      ui->edit_field==2?ui->draft.username:ui->draft.password;
    int n=(int)strlen(value);
    heading(ui->page,p,tr(ui,"EDITAR FONTE","EDIT SOURCE"),
            tr(ui,"X ABRE TECLADO NATIVO","X OPENS VITA KEYBOARD"));
    rect(252,130,672,122,p.surface);
    label(271,146,names[ui->edit_field],2,p.accent);
    label(271,190,tr(ui,"CARACTERES","CHARACTERS"),2,p.muted);
    char count[24]; snprintf(count,sizeof(count),"%d",n);
    label(424,190,count,2,p.muted);
    /* Show the tail of long URLs; the complete value remains stored. */
    const char *tail=value+(n>45?n-45:0);
    if(ui->edit_field==3) label(271,216,"PASSWORD HIDDEN",2,p.muted);
    else clipped(271,216,tail,45,2,p.white);
    for(int i=0;i<(int)(sizeof(keyboard)-1);++i) {
        int x=263+(i%11)*59, y=273+(i/11)*36;
        int active=i==ui->keyboard_index && ui->grid_keyboard;
        rect(x,y,49,30,active?p.selected:p.raised);
        if(active)stroke(x,y,49,30,p.accent);
        char ch=keyboard[i];
        if(ui->keyboard_upper && ch>='a' && ch<='z') ch=(char)(ch-'a'+'A');
        char key[2]={ch,0};
        label(x+17,y+8,key,2,active?p.white:p.muted);
    }
    if(!ui->hide_control_hints) {
        hint_button(253,467,HINT_CROSS,ui->grid_keyboard?
                    tr(ui,"LETRA","LETTER"):tr(ui,"TECLADO","KEYBOARD"),p);
        hint_button(409,467,HINT_SQUARE,tr(ui,"APAGAR","ERASE"),p);
        hint_button(552,467,HINT_LR,tr(ui,"CAMPO","FIELD"),p);
        hint_button(735,467,HINT_SELECT,tr(ui,"GRADE","GRID"),p);
        hint_button(253,501,HINT_START,tr(ui,"MAIUSC/MINUSC","UPPER/LOWER"),p);
        hint_button(522,501,HINT_TRIANGLE,tr(ui,"SALVAR","SAVE"),p);
        hint_button(714,501,HINT_CIRCLE,tr(ui,"VOLTAR","BACK"),p);
    }
    if(ui->save_status==2) label(605,146,tr(ui,"NOME E URL OBRIGATORIOS","NAME AND URL REQUIRED"),2,p.accent);
}
static void channel_browser(const IptvitaUi *ui, Palette p)
{
    heading(ui->page,p,tr(ui,"CANAIS DA PLAYLIST","PLAYLIST CHANNELS"),
            tr(ui,"CIMA/BAIXO NAVEGAR  /  O VOLTAR","UP/DOWN BROWSE  /  O BACK"));
    if(ui->playlist.status==0) label(263,150,tr(ui,"BAIXANDO PLAYLIST...","DOWNLOADING PLAYLIST..."),3,p.accent);
    if(ui->playlist.status<0) {
        const char *pt=ui->playlist.status==-1?"ERRO AO LER ARQUIVO":
            ui->playlist.status==-2?"M3U INVALIDA OU VAZIA":
            ui->playlist.status==-4?"CAMINHO LOCAL INVALIDO":
            ui->playlist.status==-13?"URL M3U EXCEDE 8191 CARACTERES":
            ui->playlist.status==-5?"PLAYLIST MAIOR QUE 4 MB":
            ui->playlist.status==-6?"ERRO HTTP DO SERVIDOR":
            ui->playlist.status==-7?"FALHA DE REDE OU HTTPS":
            ui->playlist.status==-8?"ERRO AO INICIAR REDE":
            ui->playlist.status==-9?"ERRO AO ABRIR CACHE":
            ui->playlist.status==-10?"ERRO AO INICIAR CURL":
            ui->playlist.status==-11?"ERRO AO LER CERTIFICADOS":
            ui->playlist.status==-12?"CURL NAO ACEITA CA EM MEMORIA":"LIMITE DE CANAIS";
        const char *en=ui->playlist.status==-1?"FILE READ ERROR":
            ui->playlist.status==-2?"INVALID OR EMPTY M3U":
            ui->playlist.status==-4?"INVALID LOCAL PATH":
            ui->playlist.status==-13?"M3U URL EXCEEDS 8191 CHARACTERS":
            ui->playlist.status==-5?"PLAYLIST EXCEEDS 4 MB":
            ui->playlist.status==-6?"HTTP SERVER ERROR":
            ui->playlist.status==-7?"NETWORK OR HTTPS ERROR":
            ui->playlist.status==-8?"NETWORK INITIALIZATION ERROR":
            ui->playlist.status==-9?"CACHE FILE OPEN ERROR":
            ui->playlist.status==-10?"CURL INITIALIZATION ERROR":
            ui->playlist.status==-11?"CERTIFICATE READ ERROR":
            ui->playlist.status==-12?"CURL MEMORY CA UNSUPPORTED":"CHANNEL LIMIT";
        label(263,150,ui->language?en:pt,3,p.accent);
        if(ui->playlist.status==-7 || ui->playlist.status==-6) {
            char detail[80];
            snprintf(detail,sizeof(detail),"CURL: %d  HTTP: %d",iptvita_network_error_code(),iptvita_network_http_status());
            label(263,195,detail,3,p.white);
        }
    }
    char count[40];snprintf(count,sizeof(count),"%d CANAIS",ui->playlist.count);
    label(264,117,count,2,p.muted);
    int offset=ui->channel_index>=7?ui->channel_index-6:0;
    for(int i=offset;i<ui->playlist.count && i<offset+7;++i) {
        int y=151+(i-offset)*44;
        int active=i==ui->channel_index;
        rect(252,y,672,39,active?p.selected:p.surface);
        if(active)rect(252,y,4,39,p.accent);
        clipped(267,y+10,ui->playlist.channels[i].name,37,2,p.white);
        clipped(731,y+10,ui->playlist.channels[i].group,14,2,p.muted);
    }
    label(253,491,tr(ui,"X DETALHES DO CANAL","X CHANNEL DETAILS"),2,p.muted);
    if(ui->channel_details && ui->playlist.count>0) {
        const IptvitaChannel *ch=&ui->playlist.channels[ui->channel_index];
        rect(240,108,700,400,p.bg);
        heading(ui->page,p,tr(ui,"DETALHES DO CANAL","CHANNEL DETAILS"),
                tr(ui,"O VOLTAR","O BACK"));
        clipped(265,155,ch->name,40,3,p.white);
        if(ui->stream_decoded_frames) {
            rect(272,190,640,310,RGBA8(0,0,0,255));
            iptvita_preview_draw(272,190,640,310,0);
            if(!ui->stream_info_visible)return;
            rect(272,388,640,105,RGBA8(8,20,33,225));
            char details[120];
            snprintf(details,sizeof(details),"%dx%d  FRAMES %u  DECODE %d",ui->stream_video_width,ui->stream_video_height,ui->stream_decoded_frames,ui->stream_decode_error);
            label(290,406,details,2,p.white);
            label(290,446,tr(ui,"R1 OCULTAR INFO  /  O VOLTAR","R1 HIDE INFO  /  O BACK"),2,p.accent);
            return;
        }
        const char *url=ch->url;
        const char *kind=strstr(url,".m3u8")?"HLS (.m3u8)":
                         strstr(url,".mp4")?"MP4":strstr(url,".ts")?"MPEG-TS":"FORMATO A IDENTIFICAR";
        label(265,208,kind,3,p.accent);
        label(265,256,tr(ui,"URL DO STREAM:","STREAM URL:"),2,p.muted);
        /* Long signed URLs stay intact in memory; only a small tail is shown. */
        size_t url_len=strlen(url);
        const char *shown=url+(url_len>65?url_len-65:0);
        clipped(265,284,shown,65,2,p.white);
        char url_info[64];
        snprintf(url_info,sizeof(url_info),"URL: %u / 8191 BYTES",(unsigned)url_len);
        label(265,311,url_info,2,p.muted);
        label(265,354,tr(ui,"TRI TESTAR HLS / QUAD MP4 LOCAL","TRI TEST HLS / SQUARE LOCAL MP4"),2,p.accent);
        label(265,375,tr(ui,"R1 MOSTRAR / OCULTAR INFO","R1 SHOW / HIDE INFO"),2,p.muted);
        if(ui->stream_info_visible && ui->stream_stage) {
            char line[120];
            snprintf(line,sizeof(line),"HLS ETAPA %d  STATUS %d  %s",ui->stream_stage,ui->stream_status,ui->stream_stage==-23?"DRM NAO SUPORTADO":ui->stream_cipher_method==1?"AES-128":"");
            label(265,388,line,2,p.white);
            snprintf(line,sizeof(line),"TS %u  NAL %u  AU %u  PID %d",ui->stream_packets,ui->stream_nals,ui->stream_frames,ui->stream_video_pid);
            label(265,417,line,2,p.accent);
            snprintf(line,sizeof(line),"HTTP %d  TLS/NET %d",ui->stream_http,ui->stream_net_error);
            label(265,446,line,2,p.muted);
            snprintf(line,sizeof(line),"VIDEO %u  DECODE %d",ui->stream_decoded_frames,ui->stream_decode_error);
            label(265,472,line,2,p.accent);
        }
        if(ui->stream_info_visible && ui->player_probe_result && !ui->stream_stage) {
            char probe[80];
            snprintf(probe,sizeof(probe),"%s: %d (0x%08X)",ui->player_probe_target==2?"MP4 LOCAL":"STREAM",ui->player_probe_result,(unsigned int)ui->player_probe_result);
            label(265,404,probe,2,p.white);
        }
    }
    if(ui->save_status==5)label(253,465,tr(ui,"PLAYER AINDA NAO DISPONIVEL","PLAYER NOT AVAILABLE YET"),2,p.accent);
}
static void saved_sources(const IptvitaUi *ui, Palette p)
{
    if(ui->browsing) { channel_browser(ui,p); return; }
    heading(ui->page,p,tr(ui,"FONTES SALVAS","SAVED SOURCES"),
            tr(ui,"GERENCIE SUAS FONTES","MANAGE YOUR SAVED SOURCES"));
    if(!ui->hide_control_hints) {
        hint_button(252,510,HINT_CROSS,tr(ui,"ABRIR","OPEN"),p);
        hint_button(426,510,HINT_R,tr(ui,"EDITAR","EDIT"),p);
        hint_button(642,510,HINT_SQUARE,tr(ui,"EXCLUIR","DELETE"),p);
    }
    if(!ui->sources.count)
        label(267,164,tr(ui,"NENHUMA FONTE SALVA","NO SOURCES SAVED"),3,p.muted);
    unsigned int offset=ui->list_index>=7 ? (unsigned int)(ui->list_index-6) : 0;
    for(unsigned int i=offset;i<ui->sources.count && i<offset+7;++i) {
        int y=131+(int)(i-offset)*45;
        int active=(int)i==ui->list_index;
        rect(252,y,672,39,active?p.selected:p.surface);
        if(active) rect(252,y,4,39,p.accent);
        clipped(271,y+10,ui->sources.items[i].name,27,2,p.white);
        const char *types[]={"M3U","XTREAM","DIRETO","LOCAL"};
        label(795,y+10,types[ui->sources.items[i].type],2,p.muted);
    }
    if(ui->sources.count>7 && !ui->save_status) {
        label(252,476,tr(ui,"USE CIMA/BAIXO PARA ROLAR","USE UP/DOWN TO SCROLL"),2,p.muted);
    }
    if(ui->save_status==1) label(252,476,tr(ui,"ALTERACOES SALVAS","CHANGES SAVED"),2,p.accent);
    if(ui->save_status==4) label(252,476,tr(ui,"ERRO AO SALVAR NO CARTAO","STORAGE WRITE ERROR"),2,p.accent);
    if(ui->save_status==6) label(252,476,tr(ui,"XTREAM OU ENDERECO NAO SUPORTADO","XTREAM OR UNSUPPORTED ADDRESS"),2,p.accent);
    if(ui->save_status==7) label(252,476,tr(ui,"URL INVALIDA OU DOWNLOAD OCUPADO","INVALID URL OR DOWNLOAD BUSY"),2,p.accent);
}

static void sources(const IptvitaUi *ui, Palette p)
{
    if(ui->list_mode) { saved_sources(ui,p); return; }
    static const char *names_pt[]={"M3U / M3U8","XTREAM CODES","LINK DIRETO","M3U LOCAL"};
    static const char *names_en[]={"M3U / M3U8","XTREAM CODES","DIRECT STREAM","LOCAL M3U"};
    static const char *details_pt[]={"URL DA LISTA","SERVIDOR / USUARIO / SENHA","URL DO VIDEO","ARQUIVO NO CARTAO"};
    static const char *details_en[]={"PLAYLIST URL","SERVER / USER / PASS","SINGLE VIDEO URL","FILE ON MEMORY CARD"};
    const char **names = ui->language ? names_en : names_pt;
    const char **details = ui->language ? details_en : details_pt;
    heading(ui->page,p,tr(ui,"FONTES","SOURCES"),tr(ui,"ESCOLHA O TIPO DE FONTE","CHOOSE THE FORMAT YOU WANT TO ADD"));
    for (int i=0;i<IPTVITA_SOURCE_COUNT;++i) {
        int x=252+(i%2)*342, y=131+(i/2)*153;
        int active=ui->card_index==i && ui->focus_content;
        rect(x,y,326,135,active?p.selected:p.surface);
        if(active)stroke(x,y,326,135,p.accent);
        rect(x+15,y+17,40,37,p.raised);
        label(x+27,y+30,i==0?"M":i==1?"X":i==2?"D":"F",3,p.accent);
        label(x+18,y+72,names[i],2,p.white);
        label(x+18,y+105,details[i],2,p.muted);
    }
    if(!ui->hide_control_hints) {
        hint_button(252,476,HINT_CROSS,tr(ui,"ADICIONAR","ADD"),p);
        hint_button(447,476,HINT_SELECT,tr(ui,"FONTES SALVAS","SAVED LIST"),p);
        hint_button(752,476,HINT_CIRCLE,tr(ui,"VOLTAR","BACK"),p);
    }
}
static void settings(const IptvitaUi *ui, Palette p)
{
    static const char *items_pt[]={"TEMA DE CORES","DICAS DE CONTROLES","IDIOMA"};
    static const char *items_en[]={"COLOR THEME","CONTROL HINTS","LANGUAGE"};
    const char **items=ui->language ? items_en : items_pt;
    heading(ui->page,p,tr(ui,"AJUSTES","SETTINGS"),
            tr(ui,"PERSONALIZE SUA EXPERIENCIA","VISUAL PREFERENCES AND FUTURE OPTIONS"));
    for (int i=0;i<3;++i) {
        int y=140+i*94;
        int active=ui->settings_index==i && ui->focus_content;
        rect(252,y,672,78,active?p.selected:p.surface);
        if(active) rect(252,y,4,78,p.accent);
        label(277,y+15,items[i],3,p.white);
        if (i==0) {
            const char *themes_pt[]={"AZUL ESCURO","AZUL OCEANO","INDIGO"};
            const char *themes_en[]={"DEEP BLUE","OCEAN TEAL","INDIGO"};
            label(277,y+50,ui->language?themes_en[ui->theme]:themes_pt[ui->theme],2,p.accent);
        } else if (i==1) {
            label(277,y+50,ui->hide_control_hints?
                  tr(ui,"OCULTAS","HIDDEN"):tr(ui,"VISIVEIS","VISIBLE"),2,p.accent);
        } else {
            label(277,y+50,ui->language?"ENGLISH":"PORTUGUES (BR)",2,p.accent);
        }
    }
    if(!ui->hide_control_hints) {
        hint_button(252,478,HINT_CROSS,tr(ui,"ALTERAR","CHANGE"),p);
        hint_button(489,478,HINT_CIRCLE,tr(ui,"VOLTAR","BACK"),p);
    }
}
static void about(const IptvitaUi *ui, Palette p)
{
    heading(ui->page,p,tr(ui,"SOBRE IPTVITA","ABOUT IPTVITA"),
            tr(ui,"PROJETO INDEPENDENTE E ABERTO","INDEPENDENT OPEN-SOURCE PROJECT"));
    rect(252,137,672,315,p.surface);
    rect(252,137,5,315,p.accent);
    label(282,169,"IPTVITA",5,p.white);
    label(283,221,tr(ui,"VERSAO 0.6.0 / M3U ONLINE","VERSION 0.6.0 / ONLINE M3U"),3,p.accent);
    label(284,280,tr(ui,"CRIADO POR YURIHBO","CREATED BY YURIHBO"),3,p.white);
    label(284,328,tr(ui,"LICENCA MIT  2026 YURIHBO","MIT LICENSE  2026 YURIHBO"),2,p.muted);
    label(284,361,"C / VITASDK / PLAYSTATION VITA",2,p.muted);
    if(!ui->hide_control_hints) {
        hint_button(252,488,HINT_CIRCLE,tr(ui,"VOLTAR","BACK"),p);
        hint_button(480,488,HINT_LR,tr(ui,"MUDAR TELAS","SWITCH PAGES"),p);
    }
}
static void dialog(const IptvitaUi *ui, Palette p)
{
    rect(168,152,625,241,p.bg);
    if(ui->delete_confirm) {
        stroke(168,152,625,241,p.accent);
        label(198,179,tr(ui,"EXCLUIR FONTE?","DELETE SOURCE?"),4,p.white);
        if(ui->delete_index>=0 && ui->delete_index<(int)ui->sources.count)
            clipped(200,227,ui->sources.items[ui->delete_index].name,37,2,p.muted);
        label(199,258,tr(ui,"TEM CERTEZA QUE DESEJA EXCLUIR ESTA FONTE?",
                        "ARE YOU SURE YOU WANT TO DELETE THIS SOURCE?"),2,p.white);
        rect(210,312,159,45,ui->delete_yes?p.selected:p.raised);
        rect(402,312,159,45,ui->delete_yes?p.raised:p.selected);
        if(ui->delete_yes)stroke(210,312,159,45,p.accent);
        else stroke(402,312,159,45,p.accent);
        label(259,326,tr(ui,"SIM","YES"),3,ui->delete_yes?p.white:p.muted);
        label(449,326,tr(ui,"NAO","NO"),3,ui->delete_yes?p.muted:p.white);
        hint_button(575,314,HINT_CROSS,tr(ui,"CONFIRMAR","CONFIRM"),p);
        hint_button(575,348,HINT_CIRCLE,tr(ui,"CANCELAR","CANCEL"),p);
        return;
    }
    stroke(168,152,625,241,p.accent);
    label(206,193,ui->quit_confirm?tr(ui,"SAIR DO IPTVITA?","EXIT IPTVITA?"):
                                   tr(ui,"DISPONIVEL NA ETAPA 3","COMING IN STAGE 3"),3,p.white);
    if (ui->quit_confirm) {
        label(206,263,tr(ui,"X SAIR","X EXIT APP"),3,p.accent);
        label(206,316,tr(ui,"O CONTINUAR","O KEEP BROWSING"),3,p.muted);
    } else {
        label(206,263,tr(ui,"FUNCAO EM DESENVOLVIMENTO","THIS OPTION IS A PREVIEW"),3,p.muted);
        label(206,316,tr(ui,"O VOLTAR AO MENU","O RETURN TO THE MENU"),3,p.accent);
    }
}
void iptvita_ui_render(const IptvitaUi *ui)
{
    Palette p=palette(ui->theme);
    if(ui->browsing && ui->channel_details) {
        vita2d_clear_screen();
        rect(0,0,IPTVITA_WIDTH,IPTVITA_HEIGHT,RGBA8(0,0,0,255));
        if(ui->stream_decoded_frames) iptvita_preview_draw(0,0,IPTVITA_WIDTH,IPTVITA_HEIGHT,ui->stream_fill_screen);
        else if(!ui->stream_info_visible) {
            if(ui->stream_decode_error)
                label(295,267,tr(ui,"VIDEO INDISPONIVEL - APERTE R1","VIDEO UNAVAILABLE - PRESS R1"),2,p.accent);
            else if(ui->stream_stage<0)
                label(305,267,tr(ui,"FALHA NO CANAL - APERTE R1","CHANNEL ERROR - PRESS R1"),2,p.accent);
            else
                label(395,270,tr(ui,"CARREGANDO CANAL...","LOADING CHANNEL..."),2,p.muted);
        }
        /* Show unobtrusive, measured buffer occupancy on initial tune-in or
         * when video output stops advancing, even with the R1 HUD hidden. */
        if(!ui->stream_paused && ui->stream_stage>0) {
            IptvitaPreviewStats video=iptvita_preview_stats();
            unsigned long long now=(unsigned long long)sceKernelGetProcessTimeWide();
            int stalled=video.buffering || video.decoded==0 ||
                (video.last_output_us && now>video.last_output_us+900000ULL);
            if(stalled) {
                IptvitaAudioStats audio=iptvita_audio_stats();
                int pct=video.buffer_percent;
                if(audio.played && audio.buffered<72u) {
                    int apct=(int)(audio.buffered*100u/72u);
                    if(apct<pct)pct=apct;
                }
                if(pct<0)pct=0;
                if(pct>100)pct=100;
                rect(12,12,220,50,RGBA8(8,20,33,224));
                char buffering[50];
                snprintf(buffering,sizeof(buffering),
                         video.buffering?"BUFFERING %d%%":"VIDEO %d%%",pct);
                label(23,22,buffering,2,p.white);
                rect(23,48,193,5,RGBA8(50,65,80,255));
                rect(23,48,193*pct/100,5,p.accent);
            }
        }
        if(ui->stream_info_visible) {
            rect(0,0,IPTVITA_WIDTH,87,RGBA8(8,20,33,224));
            const IptvitaChannel *ch=&ui->playlist.channels[ui->channel_index];
            clipped(25,17,ch->name,55,3,p.white);
            char details[130];
            snprintf(details,sizeof(details),"%dx%d H264 %s VIDEO %u AUDIO %s (PID %d)",ui->stream_video_width,ui->stream_video_height,ui->stream_quality_sd?"SD COMPAT":"HD AUTO",ui->stream_decoded_frames,ui->stream_audio_type==0x0f?"AAC ADTS":ui->stream_audio_type==0x11?"AAC LATM":ui->stream_audio_type==3||ui->stream_audio_type==4?"MPEG":ui->stream_audio_pid<0?"NENHUM":"OUTRO",ui->stream_audio_pid);
            label(26,56,details,2,p.accent);
            rect(0,445,IPTVITA_WIDTH,99,RGBA8(8,20,33,224));
            IptvitaAudioStats audio=iptvita_audio_stats();
            snprintf(details,sizeof(details),"HTTP %d / HLS %d / VIDEO %d (0x%08X) / AAC %u / AUDIO ERR %d",ui->stream_http,ui->stream_stage,ui->stream_decode_error,(unsigned)ui->stream_decode_error,audio.played,audio.error);
            label(25,463,details,2,p.white);
            IptvitaPreviewStats v=iptvita_preview_stats();
            snprintf(details,sizeof(details),"AVC TH %s / ES %u/%u / %u/%u MiB / RGBA %u/%u",
                     v.worker_running?"ON":"OFF",v.queued,v.queue_capacity,
                     v.queued_bytes/(1024u*1024u),v.queue_limit_mib,
                     v.decoded_queued,v.decoded_capacity);
            label(25,485,details,2,p.muted);
            /* H264 diagnostics: count B pictures and reordered output PTS. */
            {
                char extra[112];
                snprintf(extra,sizeof(extra),"FPS %u/%u CAD %ums AV %+dms",
                         v.decode_fps,v.video_fps,v.cadence_ms,
                         v.av_delta_ms);
                label(615,444,extra,2,p.accent);
            }
            {
                char cache_line[64];
                snprintf(cache_line,sizeof(cache_line),"HLS HITS %u / AVC 2PIC %u ZERO %u",
                         iptvita_network_prefetch_hits(),
                         v.avc_dual_output,v.avc_zero_output);
                label(25,444,cache_line,2,p.accent);
            }
            label(25,505,ui->stream_paused?
                  tr(ui,"PAUSADO  X CONTINUAR  R1 OCULTAR  O VOLTAR","PAUSED  X RESUME  R1 HIDE  O BACK"):
                  tr(ui,"X PAUSAR  R1 INFO  L1 TELA  O VOLTAR","X PAUSE  R1 INFO  L1 SCREEN  O BACK"),2,p.accent);
        }
        return;
    }
    vita2d_clear_screen();
    rect(0,0,IPTVITA_WIDTH,IPTVITA_HEIGHT,p.bg);
    sidebar(ui,p);
    switch (ui->page) {
        case IPTVITA_HOME: home(ui,p); break;
        case IPTVITA_SOURCES: sources(ui,p); break;
        case IPTVITA_SETTINGS: settings(ui,p); break;
        case IPTVITA_ABOUT: about(ui,p); break;
        default: home(ui,p); break;
    }
    if (ui->editing) {
        rect(222,0,738,544,p.bg);
        editor(ui,p);
    } else if (ui->show_dialog || ui->quit_confirm || ui->delete_confirm) dialog(ui,p);
}
