/* waitqueue.c - the queue of tasks waiting for something.
 *
 * A singly-linked list with a tail pointer. Adding at the tail and removing
 * from the head is what makes it first-in-first-out, and keeping the tail
 * pointer is what makes adding O(1) rather than a walk to the end.
 *
 * The only subtlety is keeping the two pointers consistent: an empty queue has
 * both at 0, and a queue of one has both pointing at the same task. Every
 * function below has to leave that true, which is why the emptiness checks look
 * slightly redundant and are not.
 */

#include "waitqueue.h"
#include "task.h"

void wait_queue_init(struct wait_queue *q)
{
	q->head = 0;
	q->tail = 0;
}

void wait_queue_add(struct wait_queue *q, struct task *t)
{
	if (!t)
		return;

	t->wait_next = 0;

	if (q->tail)
		q->tail->wait_next = t;
	else
		q->head = t;         /* was empty, so this is now both ends */

	q->tail = t;
}

struct task *wait_queue_pop(struct wait_queue *q)
{
	struct task *t = q->head;

	if (!t)
		return 0;

	q->head = t->wait_next;

	if (!q->head)
		q->tail = 0;         /* queue is empty again */

	/* Clear the link on the way out. A task carrying a stale pointer to a
	 * queue it has left is the kind of thing that looks harmless until
	 * something follows it. */
	t->wait_next = 0;

	return t;
}

void wait_queue_wake_one(struct wait_queue *q)
{
	struct task *t = wait_queue_pop(q);

	if (t)
		task_unblock(t);
}
