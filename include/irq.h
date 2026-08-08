/* irq.h — turning interrupts off for a moment, and putting them back exactly
 * as they were.
 *
 * On one CPU this is the whole of mutual exclusion. Nothing else can run to
 * interleave with us - not another task, because only an interrupt could
 * switch away, and not another interrupt handler either. That makes a section
 * of code indivisible without any lock at all, which is why the scheduler, the
 * pipes, the mutex and the console all reach for it.
 *
 * SAVE AND RESTORE, NOT OFF AND ON
 *
 * The obvious version is `cli` at the top and `sti` at the bottom, and it is
 * wrong. These regions nest: task_schedule is reached both from the timer
 * interrupt, where interrupts are already off, and from a thread calling
 * yield, where they are on. A bare `sti` at the end of the inner region would
 * turn them on in the middle of the outer one, which is exactly the situation
 * the outer one was written to prevent.
 *
 * So the previous state is read out of EFLAGS first and handed back to the
 * caller as an opaque value. Restoring means re-enabling only if they were
 * enabled before. The saved value must live somewhere per-thread - a local
 * variable on the thread's own stack - because the thread may be switched away
 * from in the middle and resumed much later.
 *
 * `static inline` in a header for the same reason as io.h: two instructions
 * pasted at the call site, and each .c file gets its own private copy rather
 * than a duplicate symbol at link time.
 */
#ifndef ARTHIC_IRQ_H
#define ARTHIC_IRQ_H

#include <stdint.h>

#define EFLAGS_IF 0x200   /* bit 9: interrupts enabled */

/* Save the interrupt flag and disable interrupts. The returned value is
 * opaque - it is the whole of EFLAGS, and only irq_restore should read it.
 *
 * `pushfl` puts EFLAGS on the stack, `popl` takes it into our variable, and
 * `cli` clears the flag. The "memory" clobber tells the compiler not to move
 * loads or stores across this, which matters: a critical section the compiler
 * has reordered instructions out of is not a critical section.
 */
static inline uint32_t irq_save(void)
{
	uint32_t flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) :: "memory");
	return flags;
}

/* Re-enable interrupts only if they were enabled when `flags` was taken. */
static inline void irq_restore(uint32_t flags)
{
	if (flags & EFLAGS_IF)
		__asm__ volatile ("sti" ::: "memory");
}

#endif
