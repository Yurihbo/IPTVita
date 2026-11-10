#ifndef IPTVITA_KEYBOARD_H
#define IPTVITA_KEYBOARD_H
#include <stddef.h>
/* Nonblocking native IME: update once per rendered frame.
 * tick: 1=editing, 0=accepted, -2=cancelled, -1=failed. */
int iptvita_keyboard_begin(char *text,size_t capacity,int is_url,int is_password,int language);
int iptvita_keyboard_tick(void);
int iptvita_keyboard_active(void);
#endif
