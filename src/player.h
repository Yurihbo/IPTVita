#ifndef IPTVITA_PLAYER_H
#define IPTVITA_PLAYER_H
/* Returns AVPlayer error code or positive probe result.
   This only checks whether the native player accepts a source. */
int iptvita_player_probe(const char *url);
#endif
