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

/* Length of a NUL-terminated string, not counting the NUL. Walking to the
 * terminator is genuinely how you find the end of a C string, so every caller
 * that wants a length was writing this loop; now they do not. */
size_t kstrlen(const char *str);

/* Copy at most `size - 1` bytes and always terminate, so the destination is a
 * valid string whatever the source was. `size` is the size of the DESTINATION,
 * which is the argument order every safe copy uses and the reason it is safe:
 * the bound belongs to the buffer, not to the data going into it.
 *
 * Returns the number of bytes written, excluding the terminator - callers that
 * need to know where the copy stopped get it without walking the result. */
size_t kstrlcpy(char *dest, const char *src, size_t size);

#endif
