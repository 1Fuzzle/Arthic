/* cpuprot.h - SMEP and SMAP, 32-bit.
 *
 * Identical mechanism to the 64-bit branch's version - two CR4 bits,
 * detected via the same CPUID leaf regardless of mode. SMEP stops the kernel
 * executing anything fetched from a page marked PAGE_USER while running at
 * CPL 0; SMAP stops it reading or writing such a page at all unless AC is
 * explicitly set via STAC first. Both exist specifically to catch the
 * kernel touching ring-3 memory in ways nothing else here can.
 */
#ifndef ARTHIC_CPUPROT_H
#define ARTHIC_CPUPROT_H

int cpuprot_init(void);          /* returns 1 if both were enabled */
int cpuprot_smep_available(void);
int cpuprot_smap_available(void);

static inline void user_access_begin(void)
{
	__asm__ volatile ("stac" ::: "memory");
}

static inline void user_access_end(void)
{
	__asm__ volatile ("clac" ::: "memory");
}

#endif
