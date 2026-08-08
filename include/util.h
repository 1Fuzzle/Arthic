/* util.h — the small arithmetic every layer was writing out by hand.
 *
 * Rounding a byte count up to whole blocks, and rounding an address to a page
 * boundary, appear in the frame allocator, the paging code, the heap, the
 * filesystem and the loader. Written inline each time they look harmless, and
 * they mostly are - but `(n + d - 1) / d` with the `- 1` forgotten is a real
 * bug that only shows up on exact multiples, which is precisely the case a
 * quick test uses.
 *
 * Macros rather than functions because they must work on any integer type,
 * including in a constant expression such as an array size. The price is that
 * each argument may be evaluated more than once, so passing something with a
 * side effect - KDIV_ROUND_UP(i++, 4) - would evaluate it twice. That hazard
 * is why the arguments are parenthesised and why the names are shouted.
 */
#ifndef ARTHIC_UTIL_H
#define ARTHIC_UTIL_H

/* How many whole `d`-sized units it takes to hold `n` of something. */
#define KDIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Round up or down to a multiple of `a`, which MUST be a power of two.
 *
 * The trick is that a power of two minus one is a mask of the bits below it, so
 * clearing those bits rounds down, and adding a - 1 first rounds up. No
 * division involved, which is why every allocator in existence insists its
 * alignments are powers of two.
 */
#define KALIGN_DOWN(n, a) ((n) & ~((a) - 1))
#define KALIGN_UP(n, a)   KALIGN_DOWN((n) + (a) - 1, (a))

#endif
