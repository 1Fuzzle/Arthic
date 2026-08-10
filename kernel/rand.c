#include "rand.h"
#include "timer.h"

static uint64_t rng_state;

void rand_init(void) {
    rng_state = timer_get_ticks() ^ 0x9E3779B97F4A7C15ULL;
    if (rng_state == 0) rng_state = 0xdeadbeefdeadbeefULL;
}

/* xorshift64 - fast, simple, deterministic given a seed.
 * NOT cryptographically secure. A real production OS would seed from
 * hardware entropy (RDRAND) or multiple timing-jitter sources. This is
 * enough to break address predictability for Arthic's ASLR, which is
 * the actual goal here - it is not meant to resist a determined attacker
 * who can observe many boots and reconstruct the seed. */
uint64_t rand_u64(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Deliberately 32-bit here, not 64-bit. A 64-bit modulo on a 32-bit build
 * needs __umoddi3, a libgcc helper we do not have (-nostdlib). Every actual
 * caller only ever needs a small range (e.g. how many pages of slack to use
 * for ASLR), so truncating to the low 32 bits of the random stream first and
 * doing ordinary 32-bit modulo avoids the missing symbol entirely. */
uint32_t rand_range(uint32_t max) {
    if (max == 0) return 0;
    uint32_t r = (uint32_t) rand_u64();
    return r % max;
}