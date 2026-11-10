#ifndef IPTVITA_STREAM_DIAGNOSTIC_H
#define IPTVITA_STREAM_DIAGNOSTIC_H
#include "ui.h"
/* Start an asynchronous network/TS diagnostic from the channel details view. */
int iptvita_stream_start(IptvitaUi *ui,const char *url);
/* Called from main thread once per frame; never blocks on network. */
void iptvita_stream_poll(IptvitaUi *ui);
void iptvita_stream_stop(IptvitaUi *ui);
#endif
