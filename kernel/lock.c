/* lock.c - a mutex.
 *
 * HOW IT WORKS
 *
 * One word: 0 means free, 1 means held. Locking is "atomically write 1 and look
 * at what was there before". If it was 0, the lock is now yours - and because
 * the swap was atomic, nobody else can have concluded the same thing. If it was
 * 1, someone already holds it, so wait.
 *
 * The atomicity is not optional and cannot be faked. Written the obvious way:
 *
 *     if (m->locked == 0)     <- timer fires here
 *         m->locked = 1;      <- and now two threads both "hold" the lock
 *
 * the check and the set are separate instructions and the gap between them is
 * exactly where everything goes wrong. `xchg` collapses both into one
 * indivisible operation, which is why the hardware has to provide it.
 *
 * WAITING BY YIELDING
 *
 * When the lock is held we call task_yield in a loop. That is a spinlock with
 * manners: it does not hog the CPU, but the waiting thread stays runnable and
 * gets scheduled repeatedly just to discover it still cannot proceed.
 *
 * A real mutex puts the waiter to sleep on a queue attached to the lock, and
 * whoever unlocks wakes it. That needs a per-lock wait queue, which is the
 * natural next step now that sleeping exists. Worth knowing this is the simple
 * version, not the finished one.
 */

#include "lock.h"
#include "task.h"

void mutex_init(struct mutex *m)
{
	m->locked    = 0;
	m->contended = 0;
}

void mutex_lock(struct mutex *m)
{
	/* atomic_xchg returns the PREVIOUS value. Non-zero means somebody else
	 * held it, so go round again. */
	while (atomic_xchg(&m->locked, 1) != 0) {
		m->contended++;
		task_yield();
	}
}

void mutex_unlock(struct mutex *m)
{
	/* A plain store is enough to release. Nobody can be mid-acquire in a way
	 * this disturbs: they are either about to swap 1 in and see the 0 we just
	 * wrote, or they already saw 1 and are waiting. */
	m->locked = 0;
}
