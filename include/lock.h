/* lock.h - mutual exclusion.
 *
 * The moment two threads touch the same data, "the code is correct" stops
 * meaning what it used to. A line like `counter++` is not one operation. It is
 * read, add, write - and the timer can fire between any two of those. Two
 * threads interleaved badly will both read the same value, both add one, and
 * both write back the same result. One increment vanishes.
 *
 * That is a data race, and the defining nastiness is that it usually does not
 * happen. It depends on exactly when the interrupt lands, so it might appear
 * once in ten thousand runs, in production, and never once while you are
 * watching.
 */
#ifndef ARTHIC_LOCK_H
#define ARTHIC_LOCK_H

#include <stdint.h>

struct task;   /* forward declaration - lock.h must not depend on task.h */

struct mutex {
	volatile uint32_t locked;
	uint32_t          contended;   /* how often someone had to wait */

	/* Threads parked on this lock, oldest first. Keeping a tail pointer makes
	 * it FIFO rather than LIFO, which matters: a stack would let a busy
	 * thread repeatedly jump the queue and starve whoever has waited
	 * longest. Fairness is a design decision, not an accident. */
	struct task *wait_head;
	struct task *wait_tail;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

/* Atomically swap a value into memory and return what was there.
 *
 * This is the primitive everything else is built from. On x86 `xchg` with a
 * memory operand asserts a bus lock automatically - it cannot be interrupted
 * partway, and no other core can slip in between the read and the write. That
 * indivisibility is the whole reason a lock can work at all: you cannot build
 * mutual exclusion out of operations that are themselves interruptible.
 */
static inline uint32_t atomic_xchg(volatile uint32_t *ptr, uint32_t value)
{
	__asm__ volatile ("xchgl %0, %1"
	                  : "+r" (value), "+m" (*ptr)
	                  :: "memory");
	return value;
}

#endif
