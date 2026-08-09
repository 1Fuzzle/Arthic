/* ssp.c - the two symbols -fstack-protector needs to exist.
 *
 * __stack_chk_guard is read by the compiler's generated prologue and compared
 * by its generated epilogue - we never call it ourselves, and neither piece
 * of generated code is visible in this file at all. That absence is the
 * point: the compiler inserts the check into EVERY function with a local
 * array, automatically, once the flag is on and this symbol exists to read.
 *
 * __stack_chk_fail is what runs when that comparison fails. There is
 * deliberately no way back from it. A smashed canary means something already
 * overwrote memory it should not have been able to reach, and by the time we
 * find out, we do not know how far that corruption went - continuing risks
 * running on a stack, or with data, that is no longer trustworthy in any way
 * we can verify. Halting is the only response that is still safe.
 */

#include "ssp.h"
#include "terminal.h"
#include <stdint.h>

/* Not a compile-time constant, and not zero. A fixed or predictable guard
 * value defeats the entire mechanism: an attacker who knows what the canary
 * will be can simply write the correct value back over it and the check never
 * fires. This does not need to be cryptographically random - only different
 * from anything a bug or a deliberate overflow would plausibly write - and it
 * only needs to be set once, at boot, before the first guarded function runs.
 */
uint64_t __stack_chk_guard = 0;

/* This function must never be protected by the mechanism it sets up.
 *
 * A protected function reads __stack_chk_guard once at entry and compares
 * against it again at exit. ssp_init's entire job is to CHANGE that global -
 * so a protected version of it would read the OLD value on the way in, write
 * a new one partway through, and then find the two do not match on the way
 * out, calling __stack_chk_fail on itself the very first time it ever runs.
 * That is exactly what happened here before this attribute was added: the
 * failure fired deep inside kernel_main, before terminal_initialise even had
 * a chance to run, so nothing about it ever reached the screen - the machine
 * simply halted in silence, looking from the outside like it had frozen
 * during early boot for no visible reason. */
__attribute__((no_stack_protector))
void ssp_init(void)
{
	/* No hardware RNG driver exists yet on this branch, so this leans on
	 * whatever entropy is easy to reach this early: the low bits of the CPU
	 * timestamp counter, mixed with a fixed pattern so a guard of exactly 0
	 * is never possible even if RDTSC somehow returned one. A hardware RNG,
	 * once one exists, is a strict improvement over this and nothing else
	 * about how the guard is used would need to change.
	 *
	 * RDTSC puts the low 32 bits in EAX and the high 32 in EDX - two separate
	 * 32-bit halves, not one wide register. The "=A" constraint some older
	 * 32-bit code uses to mean "eax:edx as a pair" does not reliably do what
	 * it looks like it does once registers are 64 bits wide, so the two
	 * halves are read explicitly and combined by hand instead. */
	uint32_t lo, hi;
	__asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
	uint64_t tsc = ((uint64_t) hi << 32) | lo;

	__stack_chk_guard = tsc ^ 0xDEADC0DECAFEBABEull;

	if (__stack_chk_guard == 0)
		__stack_chk_guard = 0xDEADC0DECAFEBABEull;
}

/* noreturn is not decoration here - it is a promise the compiler can act on.
 * Every guarded function's epilogue calls this expecting execution to never
 * come back, and marking it lets the compiler generate that epilogue without
 * a return path it will never use. */
__attribute__((noreturn))
void __stack_chk_fail(void)
{
	kprintf("\n*** STACK SMASHING DETECTED - halting immediately\n");
	kprintf("    a local buffer overran its bounds and corrupted the stack.\n");
	kprintf("    continuing would run on memory that can no longer be trusted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}
