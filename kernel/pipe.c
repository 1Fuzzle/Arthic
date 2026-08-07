/* pipe.c - a bounded channel between tasks.
 *
 * A RING BUFFER
 *
 * Fixed array, two positions, and a count. Writing advances one position,
 * reading advances the other, and both wrap around. The count is what
 * distinguishes full from empty, which two positions alone cannot - when they
 * are equal the buffer is either completely full or completely empty and there
 * is no way to tell.
 *
 * FLOW CONTROL IS THE POINT
 *
 * The buffer is deliberately small. A producer faster than its consumer fills
 * it, and then BLOCKS - it is made to wait until there is room. That
 * backpressure is the useful part: without it a fast producer either consumes
 * unbounded memory or silently drops data, and both are worse than waiting.
 *
 * Symmetrically, a reader with nothing to read blocks rather than spinning or
 * returning nothing. That is why `cat file | grep x` works without either
 * program knowing anything about the other's speed.
 *
 * INTERRUPTS RATHER THAN A MUTEX
 *
 * On one CPU, disabling interrupts is enough to make a section indivisible, and
 * it avoids a subtle problem: taking a mutex here would mean blocking while
 * holding a lock, which is where deadlocks come from. The same lost-wakeup race
 * as the mutex applies, and the same fix - test, join the queue, and block with
 * interrupts off throughout.
 */

#include "pipe.h"
#include "task.h"
#include "string.h"

void pipe_init(struct pipe *p)
{
	kmemset(p, 0, sizeof(*p));
}

static void enqueue(struct task **head, struct task **tail, struct task *t)
{
	t->wait_next = 0;

	if (*tail)
		(*tail)->wait_next = t;
	else
		*head = t;

	*tail = t;
}

static void wake_one(struct task **head, struct task **tail)
{
	struct task *t = *head;

	if (!t)
		return;

	*head = t->wait_next;
	if (!*head)
		*tail = 0;

	t->wait_next = 0;
	task_unblock(t);
}

uint32_t pipe_write(struct pipe *p, const char *data, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		for (;;) {
			uint32_t flags = irq_save();

			if (p->count < PIPE_CAPACITY) {
				p->buffer[p->write_pos] = data[i];
				p->write_pos = (p->write_pos + 1) % PIPE_CAPACITY;
				p->count++;

				/* Somebody may have been waiting for exactly this byte. */
				wake_one(&p->readers_head, &p->readers_tail);

				irq_restore(flags);
				break;
			}

			/* Full. Join the queue and stop being runnable, still with
			 * interrupts off so no reader can drain the pipe and wake an
			 * empty queue in the gap. */
			p->blocked_writes++;
			enqueue(&p->writers_head, &p->writers_tail, task_current());
			task_block();
			irq_restore(flags);
		}
	}

	return length;
}

uint32_t pipe_read(struct pipe *p, char *out, uint32_t max)
{
	uint32_t got = 0;

	while (got < max) {
		uint32_t flags = irq_save();

		if (p->count > 0) {
			out[got++] = p->buffer[p->read_pos];
			p->read_pos = (p->read_pos + 1) % PIPE_CAPACITY;
			p->count--;

			wake_one(&p->writers_head, &p->writers_tail);
			irq_restore(flags);

			/* Return as soon as we have something rather than insisting on
			 * `max` bytes. A reader that demanded a full buffer would hang
			 * whenever the writer sent less than that - which is most of the
			 * time. Real read() behaves the same way, and it is the reason
			 * callers must handle short reads. */
			continue;
		}

		if (got > 0) {
			irq_restore(flags);
			break;                    /* got something; do not wait for more */
		}

		p->blocked_reads++;
		enqueue(&p->readers_head, &p->readers_tail, task_current());
		task_block();
		irq_restore(flags);
	}

	return got;
}

uint32_t pipe_available(struct pipe *p)
{
	return p->count;
}

void pipe_stats(struct pipe *p, uint32_t *blocked_reads,
                uint32_t *blocked_writes)
{
	if (blocked_reads)  *blocked_reads  = p->blocked_reads;
	if (blocked_writes) *blocked_writes = p->blocked_writes;
}
