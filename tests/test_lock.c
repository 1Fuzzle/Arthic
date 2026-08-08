/* test_lock.c - kernel/lock.c.
 *
 * `locktest` in the shell proves a mutex prevents the race it was written to
 * prevent. It cannot show what the lock does with its wait queue, and that is
 * where the design decisions are: waiters are parked rather than spinning, the
 * queue is FIFO so nobody starves, and a woken thread re-tests the lock rather
 * than assuming it was handed over.
 *
 * atomic_xchg is a plain `xchg` instruction and runs perfectly well in a user
 * process, so the locking itself is the real thing. Only the scheduler is
 * faked.
 */
#include "arthictest.h"

#include "support/support.h"

#include "lock.h"
#include "task.h"

static struct mutex m;

static void lock_fresh(void)
{
	fake_task_reset();
	mutex_init(&m);
}

/* Stands in for the holder finishing its critical section while another
 * thread waits. */
static void unlock_hook(void *arg)
{
	mutex_unlock((struct mutex *) arg);
}

TEST(a_new_mutex_is_free)
{
	lock_fresh();

	CHECK_EQ(m.locked, 0);
	CHECK_EQ(m.contended, 0);
	CHECK(m.wait_head == NULL);
	CHECK(m.wait_tail == NULL);
}

TEST(an_uncontended_lock_costs_nothing)
{
	lock_fresh();

	mutex_lock(&m);

	CHECK_EQ(m.locked, 1);
	CHECK_EQ(m.contended, 0);
	CHECK_EQ(fake_task_block_count(), 0);   /* no queue, no context switch */

	mutex_unlock(&m);

	CHECK_EQ(m.locked, 0);
	CHECK_EQ(fake_task_unblock_count(), 0);
}

TEST(locking_twice_is_possible_after_unlocking)
{
	lock_fresh();

	mutex_lock(&m);
	mutex_unlock(&m);
	mutex_lock(&m);

	CHECK_EQ(m.locked, 1);
	CHECK_EQ(m.contended, 0);
}

TEST(a_second_locker_waits_rather_than_spinning)
{
	lock_fresh();

	mutex_lock(&m);                      /* somebody else holds it */

	/* When the second locker blocks, the hook plays the holder releasing the
	 * lock. Without the hook this call would never return, which is exactly
	 * what "blocks" means. */
	fake_task_on_block(unlock_hook, &m);

	mutex_lock(&m);

	CHECK_EQ(m.contended, 1);
	CHECK_EQ(fake_task_block_count(), 1);

	/* Re-tested and taken on the way round the loop, not handed over. */
	CHECK_EQ(m.locked, 1);
	CHECK(m.wait_head == NULL);
}

TEST(unlocking_wakes_the_longest_waiting_task_first)
{
	lock_fresh();

	/* Two tasks parked on the lock. Building the queue by hand is the honest
	 * way to test ordering without a scheduler: the queue is part of the
	 * mutex's public structure, and this is the state two blocked lockers
	 * would have left it in. */
	struct task *first  = fake_task_make("first");
	struct task *second = fake_task_make("second");

	mutex_lock(&m);

	m.wait_head = first;
	m.wait_tail = second;
	first->wait_next  = second;
	second->wait_next = NULL;

	mutex_unlock(&m);

	/* FIFO. A stack here would let a thread that just arrived jump ahead of
	 * one that has waited all along, and under load the loser never runs. */
	CHECK(fake_task_last_unblocked() == first);
	CHECK(m.wait_head == second);
	CHECK(m.wait_tail == second);
	CHECK(first->wait_next == NULL);

	mutex_unlock(&m);

	CHECK(fake_task_last_unblocked() == second);
	CHECK(m.wait_head == NULL);
	CHECK(m.wait_tail == NULL);
	CHECK_EQ(fake_task_unblock_count(), 2);
}

TEST(unlocking_an_empty_queue_wakes_nobody)
{
	lock_fresh();

	mutex_lock(&m);
	mutex_unlock(&m);

	CHECK_EQ(fake_task_unblock_count(), 0);
	CHECK_EQ(m.locked, 0);
}

TEST(interrupts_are_left_exactly_as_they_were_found)
{
	lock_fresh();

	mutex_lock(&m);
	CHECK(fake_task_interrupts_enabled());
	CHECK_EQ(fake_task_irq_depth(), 0);

	mutex_unlock(&m);
	CHECK(fake_task_interrupts_enabled());
	CHECK_EQ(fake_task_irq_depth(), 0);
}

TEST(atomic_xchg_returns_the_previous_value)
{
	/* The primitive everything above rests on. If it returned the NEW value
	 * the lock would appear free to whoever just took it, and two tasks would
	 * be inside the critical section at once. */
	volatile uint32_t cell = 0;

	CHECK_EQ(atomic_xchg(&cell, 1), 0);
	CHECK_EQ(cell, 1);

	CHECK_EQ(atomic_xchg(&cell, 7), 1);
	CHECK_EQ(cell, 7);
}

int main(void)
{
	RUN(a_new_mutex_is_free);
	RUN(an_uncontended_lock_costs_nothing);
	RUN(locking_twice_is_possible_after_unlocking);
	RUN(a_second_locker_waits_rather_than_spinning);
	RUN(unlocking_wakes_the_longest_waiting_task_first);
	RUN(unlocking_an_empty_queue_wakes_nobody);
	RUN(interrupts_are_left_exactly_as_they_were_found);
	RUN(atomic_xchg_returns_the_previous_value);

	return test_report("lock");
}
