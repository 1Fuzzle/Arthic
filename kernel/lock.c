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
 * WAITING PROPERLY
 *
 * An earlier version looped on task_yield: the waiter stayed runnable and was
 * scheduled again and again just to discover it still could not proceed. Now a
 * waiter is BLOCKED and parked on a queue hanging off the lock. It is not a
 * scheduling candidate at all until whoever holds the lock releases it and
 * wakes one waiter. Contention now costs two context switches instead of one
 * per time slice for as long as the wait lasts.
 *
 * THE LOST WAKEUP
 *
 * There is a race hiding in the obvious implementation, and it is worth
 * understanding because it is the classic one:
 *
 *     if (lock is held)          <- we check
 *                                <- holder releases it and wakes the queue,
 *                                   which is empty, so it wakes nobody
 *     block()                    <- we park on an empty queue, forever
 *
 * The window between deciding to wait and actually being on the queue is the
 * whole problem. The fix is to make "test the lock, join the queue, and block"
 * indivisible with respect to anything that could unlock. On a single CPU that
 * means interrupts off across all three, which is what irq_save gives us. The
 * scheduler restores the flags on the other side, so the waiter wakes with
 * interrupts in the state it left them.
 *
 * On a multiprocessor this would need a spinlock guarding the queue as well,
 * because another core is genuinely running at the same time and disabling
 * interrupts locally does not stop it.
 */

#include "lock.h"
#include "task.h"

void mutex_init(struct mutex *m)
{
	m->locked    = 0;
	m->contended = 0;
	m->wait_head = 0;
	m->wait_tail = 0;
}

static void enqueue(struct mutex *m, struct task *t)
{
	t->wait_next = 0;

	if (m->wait_tail)
		m->wait_tail->wait_next = t;
	else
		m->wait_head = t;

	m->wait_tail = t;
}

static struct task *dequeue(struct mutex *m)
{
	struct task *t = m->wait_head;

	if (!t)
		return 0;

	m->wait_head = t->wait_next;
	if (!m->wait_head)
		m->wait_tail = 0;

	t->wait_next = 0;
	return t;
}

void mutex_lock(struct mutex *m)
{
	for (;;) {
		uint32_t flags = irq_save();

		/* atomic_xchg returns the PREVIOUS value. Zero means it was free and
		 * is now ours. */
		if (atomic_xchg(&m->locked, 1) == 0) {
			irq_restore(flags);
			return;
		}

		m->contended++;
		enqueue(m, task_current());

		/* Still with interrupts off, so no unlock can slip between joining
		 * the queue and becoming unrunnable. task_block switches away and the
		 * scheduler restores the flags on the far side. */
		task_block();

		irq_restore(flags);

		/* Woken. Round again - the lock is not handed over directly, so
		 * another thread may have taken it in between. Re-testing rather than
		 * assuming is what keeps this correct. */
	}
}

void mutex_unlock(struct mutex *m)
{
	uint32_t flags = irq_save();

	m->locked = 0;

	struct task *waiter = dequeue(m);
	if (waiter)
		task_unblock(waiter);

	irq_restore(flags);
}
