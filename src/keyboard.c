#include "keyboard.h"
#include <psp2/libime.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>
#include <stdint.h>

/* libime has a 2048 UTF-16 character limit; the fallback editor remains
 * available for unusually long HLS links and if native IME is unavailable. */
static SceWChar16 input[2049];
static SceWChar16 initial[2049];
static unsigned char work[SCE_IME_WORK_BUFFER_SIZE] __attribute__((aligned(64)));
static volatile int finished;
static volatile int accepted;

static void ime_event(void *arg, const SceImeEventData *event)
{
    (void)arg;
    if(event->id==SCE_IME_EVENT_PRESS_ENTER) { accepted=1; finished=1; }
    else if(event->id==SCE_IME_EVENT_PRESS_CLOSE) { accepted=0; finished=1; }
}
static size_t to_utf16(const char *src,SceWChar16 *out,size_t cap)
{
    size_t i=0,n=0;
    while(src[i] && n+1<cap) {
        unsigned char a=(unsigned char)src[i++];
        uint32_t cp=a;
        if(a>=0xC2 && a<=0xDF && ((unsigned char)src[i]&0xC0)==0x80) {
            cp=((a&31)<<6)|((unsigned char)src[i++]&63);
        } else if(a>=0xE0 && a<=0xEF && ((unsigned char)src[i]&0xC0)==0x80 &&
                  src[i+1] && ((unsigned char)src[i+1]&0xC0)==0x80) {
            cp=((a&15)<<12)|(((unsigned char)src[i]&63)<<6)|((unsigned char)src[i+1]&63);
            i+=2;
        } else if(a>=0x80) cp='?';
        if(cp>0xFFFF || (cp>=0xD800 && cp<=0xDFFF)) cp='?';
        out[n++]=(SceWChar16)cp;
    }
    out[n]=0;
    return n;
}
static int from_utf16(char *dst,size_t cap,const SceWChar16 *src)
{
    size_t n=0;
    for(size_t i=0;src[i];++i) {
        uint32_t cp=src[i];
        if(cp>=0xD800 && cp<=0xDFFF) cp='?';
        int bytes=cp<0x80?1:cp<0x800?2:3;
        if(n+(size_t)bytes>=cap) return -1;
        if(bytes==1) dst[n++]=(char)cp;
        else if(bytes==2) {
            dst[n++]=(char)(0xC0|(cp>>6));
            dst[n++]=(char)(0x80|(cp&63));
        } else {
            dst[n++]=(char)(0xE0|(cp>>12));
            dst[n++]=(char)(0x80|((cp>>6)&63));
            dst[n++]=(char)(0x80|(cp&63));
        }
    }
    dst[n]=0;
    return 0;
}
int iptvita_keyboard_edit(char *text,size_t capacity,int is_url,int is_password,int language)
{
    if(!text || capacity<2) return -1;
    /* Don't truncate existing data if a URL is longer than IME capacity. */
    if(strlen(text)>=2048) return -1;
    memset(input,0,sizeof(input));
    memset(initial,0,sizeof(initial));
    to_utf16(text,initial,2049);
    finished=0; accepted=0;
    SceImeParam param;
    sceImeParamInit(&param);
    param.work=work;
    param.handler=ime_event;
    param.initialText=initial;
    param.inputTextBuffer=input;
    param.maxTextLength=capacity-1<2048?(unsigned int)(capacity-1):2048;
    param.type=is_url?SCE_IME_TYPE_URL:SCE_IME_TYPE_DEFAULT;
    param.option=SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.enterLabel=SCE_IME_ENTER_LABEL_GO;
    param.supportedLanguages=language?SCE_IME_LANGUAGE_ENGLISH:SCE_IME_LANGUAGE_PORTUGUESE_BR;
    param.languagesForced=0;
    (void)is_password;
    if(sceSysmoduleLoadModule(SCE_SYSMODULE_IME)<0) return -1;
    if(sceImeOpen(&param)<0) return -1;
    /* Native IME runs on the system overlay; no additional UI textures. */
    while(!finished) {
        if(sceImeUpdate()<0) { sceImeClose(); return -1; }
        sceKernelDelayThread(16000);
    }
    int result=-2;
    if(accepted) {
        char temp[4096];
        if(from_utf16(temp,sizeof(temp),input)==0 && strlen(temp)<capacity) {
            strcpy(text,temp);
            result=0;
        } else result=-1;
    }
    sceImeClose();
    return result;
}
