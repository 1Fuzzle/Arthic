/* cpuprot.h - SMEP and SMAP.
 *
 * Two CR4 bits, and between them they close off a whole class of privilege
 * escalation that the syscall validation in syscall.c only ever approximated
 * with software checks.
 *
 * SMEP (Supervisor Mode Execution Prevention) - the CPU refuses to EXECUTE any
 * instruction fetched from a page marked PAGE_USER while running at CPL 0.
 * Without it, a kernel bug that ends up jumping to an attacker-controlled
 * address - a corrupted function pointer, a bad return address that survived
 * whatever else should have caught it - can land in ring 3 memory and simply
 * keep running there, at full kernel privilege, executing whatever the
 * attacker put there. SMEP turns that into an immediate fault instead.
 *
 * SMAP (Supervisor Mode Access Prevention) - the CPU refuses to READ OR WRITE
 * a page marked PAGE_USER while running at CPL 0, UNLESS the AC flag in
 * RFLAGS is set. This is the more disruptive of the two, because the kernel
 * has entirely legitimate reasons to touch user memory - reading a syscall
 * argument string is exactly that. STAC sets AC (temporarily permitting the
 * access); CLAC clears it again immediately afterward. The discipline this
 * enforces is the actual point: every place the kernel touches a ring-3
 * pointer must now be an explicit, narrow, named window - "here, and only
 * here, am I deliberately reaching into user memory" - rather than an
 * ordinary pointer dereference that happens to work.
 */
#ifndef ARTHIC_CPUPROT_H
#define ARTHIC_CPUPROT_H

#include <stdint.h>

int cpuprot_init(void);          /* returns 1 if both were enabled */
int cpuprot_smep_available(void);
int cpuprot_smap_available(void);

/* Bracket exactly the accesses that must legitimately reach ring-3 memory.
 * Nest carefully if at all - these do not count, they only set/clear one
 * flag, so a stac inside a stac...clac still ends by clearing AC. */
static inline void user_access_begin(void)
{
	__asm__ volatile ("stac" ::: "memory");
}

static inline void user_access_end(void)
{
	__asm__ volatile ("clac" ::: "memory");
}

#endif
