#ifndef IPTVITA_KEYBOARD_H
#define IPTVITA_KEYBOARD_H
#include <stddef.h>
/* Native PS Vita IME; returns 0 on success, -1 if unavailable, -2 on cancel. */
int iptvita_keyboard_edit(char *text, size_t capacity, int is_url, int is_password, int language);
#endif
