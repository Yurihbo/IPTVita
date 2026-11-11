#ifndef IPTVITA_AAC_AUDIO_H
#define IPTVITA_AAC_AUDIO_H
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned queued,played,dropped,buffered;
 int primed;
 int error,rate,channels;
 uint64_t output_pts;
 int output_pts_valid;} IptvitaAudioStats;
int iptvita_audio_start(void);
void iptvita_audio_push_pes(void *user,const unsigned char *data,size_t bytes,uint64_t pts,int has_pts);
void iptvita_audio_pause(int paused);
void iptvita_audio_stop(void);
IptvitaAudioStats iptvita_audio_stats(void);
#endif
