#ifndef IPTVITA_PLAYLIST_H
#define IPTVITA_PLAYLIST_H
/* A playlist can hold thousands of channels without reserving 8192 bytes
 * per URL or placing an enormous array inside the UI singleton. */
#define IPTVITA_MAX_CHANNELS 16000
#define IPTVITA_CHANNEL_NAME 112
#define IPTVITA_CHANNEL_URL 8192
typedef struct {
 char name[IPTVITA_CHANNEL_NAME];
 char group[64];
 char *url; /* allocated to actual URL length; released on next load */
 char *tvg_id; /* optional stable M3U channel identifier, only if present */
} IptvitaChannel;
typedef struct {
 int count;
 int status; /* 0 loading, 1 loaded, -1 IO, -2 invalid, -3 count limit,
              * -4 path, -13 overlong entry, -14 out of memory */
 int capacity;
 int truncated;
 IptvitaChannel *channels;
} IptvitaPlaylist;
void iptvita_playlist_clear(IptvitaPlaylist *playlist);
int iptvita_playlist_load(IptvitaPlaylist *playlist,const char *path);
int iptvita_playlist_load_remote(IptvitaPlaylist *playlist,const char *path,
                                 const char *playlist_url);
#endif
