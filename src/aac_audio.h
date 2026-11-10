#ifndef IPTVITA_AAC_AUDIO_H
#define IPTVITA_AAC_AUDIO_H
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned queued,played,dropped;int error,rate,channels;} IptvitaAudioStats;
int iptvita_audio_start(void);
void iptvita_audio_push_pes(void *user,const unsigned char *data,size_t bytes,uint64_t pts,int has_pts);
void iptvita_audio_pause(int paused);
void iptvita_audio_stop(void);
IptvitaAudioStats iptvita_audio_stats(void);
#endif
