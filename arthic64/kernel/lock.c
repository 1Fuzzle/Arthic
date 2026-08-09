/* lock.c - a mutex, 64-bit. Direct port of the 32-bit branch's design; see
 * that file's comments for the lost-wakeup race this avoids and why waiting
 * must happen with interrupts off across test-enqueue-block. Nothing about
 * that reasoning depended on word size.
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
		uint64_t flags = irq_save();

		if (atomic_xchg(&m->locked, 1) == 0) {
			irq_restore(flags);
			return;
		}

		m->contended++;
		enqueue(m, task_current());

		task_block();
		irq_restore(flags);
	}
}

void mutex_unlock(struct mutex *m)
{
	uint64_t flags = irq_save();

	m->locked = 0;

	struct task *waiter = dequeue(m);
	if (waiter)
		task_unblock(waiter);

	irq_restore(flags);
}
