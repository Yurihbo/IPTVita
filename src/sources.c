#include "sources.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include <stdint.h>

#define SOURCES_DIR "ux0:data/IPTVita"
#define SOURCES_FILE SOURCES_DIR "/sources.bin"
#define SOURCES_TMP SOURCES_DIR "/sources.tmp"
#define SOURCES_MAGIC 0x49505433u
#define SOURCES_VERSION 1u

typedef struct {
    uint32_t magic, version, count;
} SourceHeader;

static int valid(const IptvitaSource *item)
{
    return item->type < 4 &&
           memchr(item->name, 0, sizeof(item->name)) != 0 &&
           memchr(item->url, 0, sizeof(item->url)) != 0 &&
           memchr(item->username, 0, sizeof(item->username)) != 0 &&
           memchr(item->password, 0, sizeof(item->password)) != 0;
}
int iptvita_sources_load(IptvitaSources *s)
{
    memset(s, 0, sizeof(*s));
    SceUID fd = sceIoOpen(SOURCES_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    SourceHeader h;
    int ok = sceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h) &&
             h.magic == SOURCES_MAGIC && h.version == SOURCES_VERSION &&
             h.count <= IPTVITA_MAX_SOURCES;
    if (ok) {
        for (uint32_t i=0; i<h.count; ++i) {
            if (sceIoRead(fd, &s->items[i], sizeof(IptvitaSource)) != (int)sizeof(IptvitaSource) ||
                !valid(&s->items[i])) { ok = 0; break; }
        }
    }
    sceIoClose(fd);
    if (!ok) { memset(s, 0, sizeof(*s)); return -1; }
    s->count = h.count;
    return 0;
}
int iptvita_sources_save(const IptvitaSources *s)
{
    if (s->count > IPTVITA_MAX_SOURCES) return -1;
    sceIoMkdir(SOURCES_DIR, 0777);
    SceUID fd = sceIoOpen(SOURCES_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) return -1;
    SourceHeader h = { SOURCES_MAGIC, SOURCES_VERSION, s->count };
    int ok = sceIoWrite(fd, &h, sizeof(h)) == (int)sizeof(h);
    for (unsigned int i=0; ok && i<s->count; ++i)
        ok = sceIoWrite(fd, &s->items[i], sizeof(IptvitaSource)) == (int)sizeof(IptvitaSource);
    sceIoClose(fd);
    if (!ok) { sceIoRemove(SOURCES_TMP); return -1; }
    sceIoRemove(SOURCES_FILE);
    if (sceIoRename(SOURCES_TMP, SOURCES_FILE) < 0) return -1;
    return 0;
}
int iptvita_sources_add(IptvitaSources *s, const IptvitaSource *item)
{
    if (s->count >= IPTVITA_MAX_SOURCES || !valid(item)) return -1;
    s->items[s->count++] = *item;
    return 0;
}
int iptvita_sources_remove(IptvitaSources *s, unsigned int index)
{
    if (index >= s->count) return -1;
    for (unsigned int i=index; i+1<s->count; ++i) s->items[i] = s->items[i+1];
    memset(&s->items[--s->count], 0, sizeof(IptvitaSource));
    return 0;
}
