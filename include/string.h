/* string.h — the handful of standard library functions we need and must
 * therefore write ourselves.
 *
 * Deliberately not named after the real <string.h>. This one lives in
 * include/ and is found by quotes, so there is no ambiguity — but a reader
 * should still know at a glance that these are ours.
 */
#ifndef ARTHIC_STRING_H
#define ARTHIC_STRING_H

#include <stddef.h>

void *kmemset(void *dest, int value, size_t count);
void *kmemcpy(void *dest, const void *src, size_t count);
int   kstrcmp(const char *a, const char *b);
int   kstartswith(const char *str, const char *prefix);

#endif
