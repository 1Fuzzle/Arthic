/* waitqueue.h — a FIFO queue of blocked tasks.
 *
 * Both the mutex and the pipe need the same thing: somewhere to park a task
 * that cannot proceed, and a way to wake the one that has waited longest. They
 * each had their own copy of the enqueue and dequeue code, differing only in
 * which fields the links lived in. This is that code, once.
 *
 * INTRUSIVE, AND THAT IS THE POINT
 *
 * The link lives in the task itself - `wait_next` - rather than in a node the
 * queue allocates. That means putting a task on a queue cannot fail and cannot
 * block, which matters enormously here: the code doing it is running with
 * interrupts off, and calling kmalloc from there would be asking for trouble.
 * Kernel data structures are intrusive for exactly this reason.
 *
 * The cost is that a task can be on only one wait queue at a time. That is
 * fine, because a task waiting for two things at once is not something this
 * kernel can express anyway.
 *
 * FIFO, NOT LIFO
 *
 * The tail pointer is what makes this fair. Without it the newest waiter would
 * be woken first, and a busy task could jump the queue repeatedly and starve
 * whoever has waited longest. Fairness is a design decision, and this is where
 * it is made.
 *
 * NO LOCKING IN HERE
 *
 * These functions are not safe on their own. Every caller must already be
 * inside an interrupts-off region, because the whole point is to test a
 * condition, join the queue and block without anything slipping in between -
 * the lost wakeup described in lock.c. Putting irq_save in here would look
 * safer and would not be: the critical section has to be wider than the queue
 * operation.
 */
#ifndef ARTHIC_WAITQUEUE_H
#define ARTHIC_WAITQUEUE_H

struct task;

struct wait_queue {
	struct task *head;   /* oldest waiter - woken first */
	struct task *tail;
};

void wait_queue_init(struct wait_queue *q);

/* Put `t` at the back of the queue. Does not block it; the caller does that,
 * still with interrupts off. */
void wait_queue_add(struct wait_queue *q, struct task *t);

/* Take the oldest waiter off the queue, or 0 if there is none. */
struct task *wait_queue_pop(struct wait_queue *q);

/* Take the oldest waiter off the queue and make it runnable again. Does
 * nothing at all if nobody is waiting, which is the common case. */
void wait_queue_wake_one(struct wait_queue *q);

#endif
