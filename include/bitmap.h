/* bitmap.h — one bit per thing.
 *
 * The frame allocator tracks which 4 KB frames of RAM are in use; the
 * filesystem tracks which 512-byte disk blocks are in use. Different things,
 * identical bookkeeping, and each had its own copy of the same three lines of
 * bit arithmetic.
 *
 * THE ARITHMETIC
 *
 * Bit N lives in byte N / 8, at position N % 8 within that byte. `1 << (n % 8)`
 * builds a mask with only that bit set: OR it in to set the bit, AND with its
 * complement to clear it, AND with it to test it. That is the whole idea, and it
 * is worth being able to read it without thinking, because bit arithmetic like
 * this is everywhere below the level of a language's data structures.
 *
 * The compiler turns / 8 into a shift and % 8 into a mask, so writing it the
 * clear way costs nothing. Write it clearly; let the compiler be clever.
 *
 * BYTES, NOT WORDS
 *
 * Addressing bytes rather than 32-bit words means the size of a bitmap in
 * memory is just "however many bits, rounded up to bytes", with no alignment or
 * endianness questions. A word-at-a-time version can scan for a free bit faster
 * by skipping full words; that is an optimisation to make when a scan is
 * measurably slow, and neither user here is close.
 */
#ifndef ARTHIC_BITMAP_H
#define ARTHIC_BITMAP_H

#include <stdint.h>

static inline void bitmap_set(uint8_t *bits, uint32_t index)
{
	bits[index / 8] |= (uint8_t)(1u << (index % 8));
}

static inline void bitmap_clear(uint8_t *bits, uint32_t index)
{
	bits[index / 8] &= (uint8_t) ~(1u << (index % 8));
}

static inline int bitmap_test(const uint8_t *bits, uint32_t index)
{
	return (bits[index / 8] >> (index % 8)) & 1;
}

/* Set or clear according to a flag, since callers usually have the state in a
 * variable rather than in the choice of call. */
static inline void bitmap_assign(uint8_t *bits, uint32_t index, int value)
{
	if (value)
		bitmap_set(bits, index);
	else
		bitmap_clear(bits, index);
}

#endif
