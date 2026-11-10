#ifndef IPTVITA_SOURCES_H
#define IPTVITA_SOURCES_H
#define IPTVITA_MAX_SOURCES 16
#define IPTVITA_URL_MAX 8192
#define IPTVITA_NAME_MAX 48
#define IPTVITA_USER_MAX 96
typedef struct {
    char name[IPTVITA_NAME_MAX];
    char url[IPTVITA_URL_MAX];
    char username[IPTVITA_USER_MAX];
    char password[IPTVITA_USER_MAX];
    unsigned int type;
} IptvitaSource;
typedef struct {
    unsigned int count;
    IptvitaSource items[IPTVITA_MAX_SOURCES];
} IptvitaSources;
int iptvita_sources_load(IptvitaSources *s);
int iptvita_sources_save(const IptvitaSources *s);
int iptvita_sources_add(IptvitaSources *s, const IptvitaSource *item);
int iptvita_sources_remove(IptvitaSources *s, unsigned int index);
#endif
