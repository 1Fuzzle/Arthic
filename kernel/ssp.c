/* ssp.c - the two symbols -fstack-protector needs, 32-bit.
 *
 * Same two symbols, same reasoning, as the 64-bit branch's version:
 * __stack_chk_guard is read at the entry of every guarded function and
 * compared again at exit; __stack_chk_fail runs when that comparison
 * disagrees, and there is deliberately no way back from it - a smashed
 * canary means something already wrote where it should not have been able
 * to, and by the time we know that, we cannot know how far it went.
 */

#include "ssp.h"
#include "terminal.h"
#include <stdint.h>

/* Not a compile-time constant, and not zero - a fixed or predictable guard
 * defeats the entire mechanism, since an attacker who can predict it can
 * simply write the correct value back over it. */
uint32_t __stack_chk_guard = 0;

/* This function must never be protected by the mechanism it sets up - see
 * the identical note on the 64-bit branch's ssp.c. A protected copy of THIS
 * function would read the OLD guard value at its own entry, overwrite it
 * partway through its own body, and find the two disagree at its own exit -
 * calling __stack_chk_fail on itself the very first time it ever runs, in
 * total silence, before terminal_initialise has even had a chance to print
 * anything. */
__attribute__((no_stack_protector))
void ssp_init(void)
{
	/* No hardware RNG driver exists on this branch, so this leans on the
	 * timestamp counter's low bits, mixed with a fixed pattern so a guard
	 * of exactly 0 - which would defeat nothing, but is also not a value
	 * RDTSC would plausibly hand back on its own - is never possible.
	 *
	 * RDTSC's low 32 bits land in EAX directly on a 32-bit build - there is
	 * no separate high half to combine, unlike the 64-bit branch, which has
	 * to assemble EDX:EAX by hand. */
	uint32_t tsc;
	__asm__ volatile ("rdtsc" : "=a" (tsc));

	__stack_chk_guard = tsc ^ 0xCAFEBABEu;

	if (__stack_chk_guard == 0)
		__stack_chk_guard = 0xCAFEBABEu;
}

__attribute__((noreturn))
void __stack_chk_fail(void)
{
	kprintf("\n*** STACK SMASHING DETECTED - halting immediately\n");
	kprintf("    a local buffer overran its bounds and corrupted the stack.\n");
	kprintf("    continuing would run on memory that can no longer be trusted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}
