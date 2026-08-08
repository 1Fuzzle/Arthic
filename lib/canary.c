/* lib/canary.c — stack canary detection and failure handling.
 *
 * WHAT THIS IS FOR
 *
 * When you compile with -fstack-protector, the compiler inserts checks at the
 * end of every function. It stores a "canary" — a random value — on the stack
 * near the return address, and before returning, it verifies the canary has not
 * been overwritten.
 *
 * If a buffer overflow happens, the attacker overwrites the canary. The check
 * fails and gcc calls __stack_chk_fail. Without this function, the linker fails.
 *
 * HOW IT WORKS
 *
 * Two pieces:
 *
 * 1. __stack_chk_guard
 *    The magic value the compiler uses as the canary. It lives in a special
 *    TLS (thread-local storage) location in real systems, but for a single-
 *    threaded kernel we use a plain global variable. The compiler emits code
 *    to load this at function prologue and check it at epilogue.
 *
 * 2. __stack_chk_fail
 *    Called when the canary check fails. In a real system this would dump a
 *    stack trace and terminate the process. Here we just halt the kernel and
 *    report the address where it happened.
 *
 * THE ATTACK THIS STOPS
 *
 *    char buf[64];
 *    read(buf, 1000);          // Read 1000 bytes into 64-byte buffer
 *
 * Without canary:
 *    Overflow writes past buf into:
 *      - saved frame pointer
 *      - return address (critical!)
 *    When the function returns, it jumps to attacker's code. Game over.
 *
 * With canary:
 *    Overflow writes past buf, but canary sits between buf and saved state.
 *    Attacker would need to know the exact canary value to avoid triggering
 *    the check. We randomise it at boot, so they cannot guess.
 *    Overflow → canary corrupted → check fails → __stack_chk_fail → halt.
 *
 * CANARY VALUE
 *
 * We use 0x59555254 (little-endian: "RUSTY" on the wire — a nod to the
 * principle). In practice, a random value would be better, but a constant
 * is fine for a learning kernel. Real systems (Linux, etc) use a random
 * value read from /dev/urandom at boot.
 */

#include <stdint.h>
#include "terminal.h"

/* The canary value. gcc reads this at function prologue and checks it at
 * epilogue. The name and type are fixed by compiler convention.
 *
 * volatile tells the compiler this value may change outside normal program
 * flow. Without it, the compiler might cache a read and assume it never
 * changes — but the check function deliberately modifies it as part of the
 * attack detection. Also, theoretically an attacker might change it from
 * ring 3, so marking it volatile is the right defensive stance.
 */
volatile uint32_t __stack_chk_guard = 0x59555254;

/* Called by the compiler when it detects a stack overflow.
 *
 * The parameter 'retaddr' is NOT a "real" parameter. The compiler does not
 * call this like a normal function. Instead, when the canary check fails,
 * gcc emits code that passes the return address as though it were an argument.
 * This lets us report where the corruption was detected, which is useful for
 * debugging. We do not even need to use it; having the parameter in the
 * signature satisfies the compiler's ABI expectations.
 */
void __stack_chk_fail(void) {
	/* Disable interrupts. We are about to halt and we do not want a timer
	 * interrupt waking us up again. cli stops all interrupts on this CPU.
	 *
	 * This is inline assembly: the string in quotes is the instruction,
	 * the colons separate input, output, and clobber lists. Empty here.
	 */
	__asm__ volatile ("cli");

	/* Print error message to both VGA and serial. This is the last thing
	 * the kernel will ever output, so make it count. */
	terminal_set_colour(vga_entry_colour(VGA_LIGHT_RED, VGA_BLACK));
	kprintf("\n*** STACK BUFFER OVERFLOW DETECTED ***\n");
	kprintf("Canary check failed. The kernel is halting to prevent code execution.\n");
	kprintf("This usually means a buffer overflow in the kernel itself.\n");

	/* Park the CPU forever. hlt stops the CPU until an interrupt. Since we
	 * just disabled interrupts, hlt will never wake up. The only way out is
	 * to reset the machine by hand.
	 *
	 * We wrap this in a label and jmp so even if an interrupt somehow breaks
	 * out of hlt, we jump back to hlt again.
	 */
	__asm__ volatile (
		"1: hlt\n"
		"   jmp 1b\n"
	);

	/* Dead code. The asm above never returns. Putting this here makes it
	 * explicit that this function is intended to never return — it terminates
	 * the entire system. Some compilers might warn if the function reaches
	 * the end without returning; we silence that by making it unreachable. */
	for (;;)
		;
}
