#ifndef IPTVITA_FAVORITES_H
#define IPTVITA_FAVORITES_H
#include "ui.h"
/* Favorites are keyed by BOTH source URL and channel URL.
 * Entries survive a playlist refresh or source reordering. */
void iptvita_favorites_load(void);
int iptvita_favorites_has(const char *source_url,const IptvitaChannel *channel);
/* Returns 1 added, 0 removed, negative if storage is unavailable/full. */
int iptvita_favorites_toggle(const char *source_url,const IptvitaChannel *channel);
/* Rebuild a compact index of favorite channels in the loaded playlist. */
void iptvita_favorites_rebuild(IptvitaUi *ui);
#endif
