/* pipe.c - a bounded channel between tasks, 64-bit port.
 *
 * Same ring buffer, same backpressure, same reasoning as the 32-bit branch:
 * a full pipe blocks the writer rather than dropping data or growing without
 * limit, and an empty one blocks the reader rather than spinning. Interrupts
 * off across test-enqueue-block for the same lost-wakeup reason as the mutex.
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

uint64_t pipe_write(struct pipe *p, const char *data, uint64_t length)
{
	for (uint64_t i = 0; i < length; i++) {
		for (;;) {
			uint64_t flags = irq_save();

			if (p->count < PIPE_CAPACITY) {
				p->buffer[p->write_pos] = data[i];
				p->write_pos = (p->write_pos + 1) % PIPE_CAPACITY;
				p->count++;

				wake_one(&p->readers_head, &p->readers_tail);

				irq_restore(flags);
				break;
			}

			p->blocked_writes++;
			enqueue(&p->writers_head, &p->writers_tail, task_current());
			task_block();
			irq_restore(flags);
		}
	}

	return length;
}

uint64_t pipe_read(struct pipe *p, char *out, uint64_t max)
{
	uint64_t got = 0;

	while (got < max) {
		uint64_t flags = irq_save();

		if (p->count > 0) {
			out[got++] = p->buffer[p->read_pos];
			p->read_pos = (p->read_pos + 1) % PIPE_CAPACITY;
			p->count--;

			wake_one(&p->writers_head, &p->writers_tail);
			irq_restore(flags);
			continue;
		}

		if (got > 0) {
			irq_restore(flags);
			break;
		}

		p->blocked_reads++;
		enqueue(&p->readers_head, &p->readers_tail, task_current());
		task_block();
		irq_restore(flags);
	}

	return got;
}

uint64_t pipe_available(struct pipe *p)
{
	return p->count;
}

void pipe_stats(struct pipe *p, uint64_t *blocked_reads,
                uint64_t *blocked_writes)
{
	if (blocked_reads)  *blocked_reads  = p->blocked_reads;
	if (blocked_writes) *blocked_writes = p->blocked_writes;
}
