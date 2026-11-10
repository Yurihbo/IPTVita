#ifndef IPTVITA_PLAYLIST_H
#define IPTVITA_PLAYLIST_H
#define IPTVITA_MAX_CHANNELS 300
#define IPTVITA_CHANNEL_NAME 112
#define IPTVITA_CHANNEL_URL 8192
typedef struct {
 char name[IPTVITA_CHANNEL_NAME];
 char url[IPTVITA_CHANNEL_URL];
 char group[64];
} IptvitaChannel;
typedef struct {
 int count;
 int status; /* 0 idle, 1 loaded, -1 IO, -2 invalid/empty, -3 truncated, -4 unsupported */
 IptvitaChannel channels[IPTVITA_MAX_CHANNELS];
} IptvitaPlaylist;
int iptvita_playlist_load(IptvitaPlaylist *playlist,const char *path);
#endif
