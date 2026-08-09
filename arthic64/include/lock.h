/* lock.h - mutual exclusion, 64-bit.
 *
 * Identical idea to the 32-bit branch: xchg on a memory operand is
 * indivisible on x86 regardless of register width, so the primitive that
 * makes a lock work at all needs no porting beyond the type.
 */
#ifndef ARTHIC_LOCK_H
#define ARTHIC_LOCK_H

#include <stdint.h>

struct task;

struct mutex {
	volatile uint64_t locked;
	uint64_t          contended;
	struct task      *wait_head;
	struct task      *wait_tail;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

static inline uint64_t atomic_xchg(volatile uint64_t *ptr, uint64_t value)
{
	__asm__ volatile ("xchgq %0, %1"
	                  : "+r" (value), "+m" (*ptr)
	                  :: "memory");
	return value;
}

#endif
