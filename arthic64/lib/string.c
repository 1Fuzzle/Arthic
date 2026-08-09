/* string.c — our own string and memory routines.
 *
 * These exist because there is no libc. Every one of them is a loop you could
 * write yourself, and that is rather the point: nothing here is magic, it is
 * just that somebody has to do it.
 *
 * Real implementations copy 4 or 16 bytes at a time and are considerably
 * faster. Ours are byte-at-a-time and obviously correct, which is the right
 * trade for now. Optimise when something is actually slow, not before.
 */

#include "string.h"

/* Fill `count` bytes with `value`.
 *
 * `value` is an int rather than a char purely because the real memset is
 * declared that way — a historical wart worth matching so the signature is
 * familiar. Only the low byte is used, hence the cast.
 */
void *kmemset(void *dest, int value, size_t count)
{
	unsigned char *d = (unsigned char *) dest;
	while (count--)
		*d++ = (unsigned char) value;
	return dest;
}

/* Copy `count` bytes forward.
 *
 * Forward-only, so this is memcpy and not memmove: if the regions overlap and
 * dest is above src, this corrupts the data. Use it only where you know they
 * do not overlap. terminal_scroll deals with the same hazard and solves it the
 * same way — by choosing the direction deliberately.
 */
void *kmemcpy(void *dest, const void *src, size_t count)
{
	unsigned char       *d = (unsigned char *) dest;
	const unsigned char *s = (const unsigned char *) src;
	while (count--)
		*d++ = *s++;
	return dest;
}

/* 0 when equal, matching the real strcmp's convention. */
int kstrcmp(const char *a, const char *b)
{
	while (*a && (*a == *b)) {
		a++;
		b++;
	}
	return (int)((unsigned char) *a) - (int)((unsigned char) *b);
}

int kstartswith(const char *str, const char *prefix)
{
	while (*prefix) {
		if (*str != *prefix)
			return 0;
		str++;
		prefix++;
	}
	return 1;
}
