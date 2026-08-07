/* timer.c — the Programmable Interval Timer.
 *
 * IRQ 0 fires roughly 18.2 times a second by default. Counting those ticks is
 * the simplest possible clock, and it is what everything time-based later will
 * sit on: sleeping, scheduling, timeouts.
 */

#include <stdint.h>

#include "timer.h"
#include "idt.h"

/* ---- Timer ----------------------------------------------------------------
 * IRQ 0 fires roughly 18.2 times a second by default. This handler does the
 * simplest possible useful thing: count, and say something once a second.
 *
 * `volatile` on the counter tells the compiler this variable changes outside
 * normal program flow. Without it, a loop that only reads `ticks` could be
 * optimised into reading it once and assuming it never changes — because as
 * far as the compiler can see, nothing ever writes to it. Any variable shared
 * between an interrupt handler and normal code needs this.
 */
static volatile uint32_t ticks = 0;

static void timer_handler(struct registers *regs) {
	(void) regs;   /* unused — this silences the -Wextra warning honestly,
	                * rather than by removing the parameter we may want later */
	ticks++;
}

/* The counter is static, so anything outside this file goes through here.
 * That is deliberate: one place writes it, everything else only reads. */
uint32_t timer_get_ticks(void) {
	return ticks;
}


/* Register the handler and unmask IRQ 0. */
void timer_install(void) {
	irq_install_handler(0, timer_handler);
}
