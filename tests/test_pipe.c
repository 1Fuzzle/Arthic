/* test_pipe.c - kernel/pipe.c.
 *
 * A ring buffer is easy to write and easy to write wrongly, and the two ways
 * it goes wrong - losing a byte at the wrap, and confusing full with empty -
 * both need the buffer to be driven past its capacity before they show. The
 * shell's `pipetest` moves a few short strings between two tasks and never
 * gets near either case.
 *
 * The blocking paths are the harder half. They only execute when a producer
 * outruns a consumer, and in the kernel that depends on the timer. Here the
 * fake task_block runs a hook (see tests/support/fake_task.c) that plays the
 * other party, so "the pipe is full and the writer must wait" happens exactly
 * when the test says it does.
 */
#include <string.h>

#include "arthictest.h"

#include "support/support.h"

#include "pipe.h"
#include "task.h"

static struct pipe p;

static void pipe_fresh(void)
{
	fake_task_reset();
	pipe_init(&p);
}

/* Hooks standing in for the task at the other end of the pipe. Both call the
 * real pipe functions rather than poking the ring directly, so the wake-ups
 * happen for real - which is the half of the protocol worth testing. Neither
 * can recurse into blocking: the drain runs only when the pipe is full and the
 * feed only when it is empty. */

struct drain {
	uint32_t bytes;
	char     buffer[512];
	uint32_t got;
};

static void drain_hook(void *arg)
{
	struct drain *d = arg;

	d->got = pipe_read(&p, d->buffer, d->bytes);
}

struct feed {
	const char *data;
	uint32_t    length;
};

static void feed_hook(void *arg)
{
	struct feed *f = arg;

	pipe_write(&p, f->data, f->length);
}

TEST(a_new_pipe_is_empty)
{
	pipe_fresh();

	CHECK_EQ(pipe_available(&p), 0);

	uint32_t reads = 99, writes = 99;
	pipe_stats(&p, &reads, &writes);
	CHECK_EQ(reads, 0);
	CHECK_EQ(writes, 0);
}

TEST(bytes_come_out_in_the_order_they_went_in)
{
	pipe_fresh();

	CHECK_EQ(pipe_write(&p, "hello", 5), 5);
	CHECK_EQ(pipe_available(&p), 5);

	char out[8] = { 0 };
	CHECK_EQ(pipe_read(&p, out, 5), 5);
	CHECK_STR_EQ(out, "hello");
	CHECK_EQ(pipe_available(&p), 0);
}

TEST(a_read_returns_what_is_there_rather_than_waiting_for_more)
{
	pipe_fresh();

	pipe_write(&p, "abc", 3);

	char out[16] = { 0 };

	/* Asked for 16, only 3 available. A short read is correct - insisting on
	 * the full count is how a reader deadlocks against a writer that has
	 * nothing more to say. */
	CHECK_EQ(pipe_read(&p, out, sizeof(out)), 3);
	CHECK_MEM_EQ(out, "abc", 3);
	CHECK_EQ(fake_task_block_count(), 0);   /* and it never blocked */
}

TEST(reading_zero_bytes_returns_immediately)
{
	pipe_fresh();

	char out[1];

	/* An empty pipe and a zero-length read: the loop must not run at all,
	 * or the reader would block waiting for data it never asked for. */
	CHECK_EQ(pipe_read(&p, out, 0), 0);
	CHECK_EQ(fake_task_block_count(), 0);
}

TEST(writing_zero_bytes_does_nothing)
{
	pipe_fresh();

	CHECK_EQ(pipe_write(&p, "ignored", 0), 0);
	CHECK_EQ(pipe_available(&p), 0);
}

TEST(the_ring_wraps_without_losing_or_reordering_bytes)
{
	pipe_fresh();

	char pattern[PIPE_CAPACITY];
	for (int i = 0; i < PIPE_CAPACITY; i++)
		pattern[i] = (char)('A' + (i % 26));

	/* Fill most of the ring, drain most of it, then write enough to carry the
	 * write position past the end of the array. Anything that mishandles the
	 * wrap corrupts the second batch. */
	pipe_write(&p, pattern, 200);

	char first[200];
	CHECK_EQ(pipe_read(&p, first, 200), 200);
	CHECK_MEM_EQ(first, pattern, 200);

	pipe_write(&p, pattern, 100);        /* write_pos wraps around here */

	char second[100];
	CHECK_EQ(pipe_read(&p, second, 100), 100);
	CHECK_MEM_EQ(second, pattern, 100);

	CHECK_EQ(pipe_available(&p), 0);
	CHECK_EQ(fake_task_block_count(), 0);
}

TEST(a_full_pipe_holds_exactly_its_capacity)
{
	pipe_fresh();

	char pattern[PIPE_CAPACITY];
	memset(pattern, 'x', sizeof(pattern));

	pipe_write(&p, pattern, PIPE_CAPACITY);

	/* Full and empty both leave read_pos == write_pos. The count is what
	 * tells them apart, and this is the state where getting that wrong turns
	 * a full pipe into an empty one. */
	CHECK_EQ(pipe_available(&p), PIPE_CAPACITY);
	CHECK_EQ(p.read_pos, p.write_pos);
	CHECK_EQ(fake_task_block_count(), 0);   /* filling it exactly is not a wait */
}

TEST(a_writer_blocks_when_the_pipe_is_full_and_resumes_after_a_read)
{
	pipe_fresh();

	char pattern[PIPE_CAPACITY];
	memset(pattern, 'y', sizeof(pattern));
	pipe_write(&p, pattern, PIPE_CAPACITY);

	static struct drain d;
	memset(&d, 0, sizeof(d));
	d.bytes = 10;                        /* the reader takes ten bytes */
	fake_task_on_block(drain_hook, &d);

	CHECK_EQ(pipe_write(&p, "0123456789", 10), 10);

	/* Backpressure, which is the entire reason the pipe is bounded: the
	 * writer waited rather than dropping data or growing the buffer. */
	CHECK_EQ(fake_task_block_count(), 1);
	CHECK_EQ(d.got, 10);

	/* The read that made room also took the writer off the queue. */
	CHECK(p.writers_head == NULL);
	CHECK(p.writers_tail == NULL);
	CHECK_EQ(fake_task_unblock_count(), 1);

	uint32_t blocked_reads = 0, blocked_writes = 0;
	pipe_stats(&p, &blocked_reads, &blocked_writes);
	CHECK_EQ(blocked_writes, 1);
	CHECK_EQ(blocked_reads, 0);

	CHECK_EQ(pipe_available(&p), PIPE_CAPACITY);
}

TEST(a_reader_blocks_on_an_empty_pipe_until_a_writer_arrives)
{
	pipe_fresh();

	static struct feed f;
	f.data = "late";
	f.length = 4;
	fake_task_on_block(feed_hook, &f);

	char out[8] = { 0 };
	CHECK_EQ(pipe_read(&p, out, 4), 4);
	CHECK_MEM_EQ(out, "late", 4);

	CHECK_EQ(fake_task_block_count(), 1);

	uint32_t blocked_reads = 0, blocked_writes = 0;
	pipe_stats(&p, &blocked_reads, &blocked_writes);
	CHECK_EQ(blocked_reads, 1);
}

TEST(a_write_wakes_a_waiting_reader)
{
	pipe_fresh();

	struct task *reader = task_current();

	static struct feed f;
	f.data = "z";
	f.length = 1;
	fake_task_on_block(feed_hook, &f);

	char out[2] = { 0 };
	pipe_read(&p, out, 1);

	/* The reader queued itself before blocking, so the byte arriving must
	 * take it off the queue - a byte written into a pipe with a queued reader
	 * and no wake-up is the lost-wakeup bug, and the pipe would hang. */
	CHECK(p.readers_head == NULL);
	CHECK(p.readers_tail == NULL);
	CHECK_EQ(fake_task_unblock_count(), 1);
	CHECK(fake_task_last_unblocked() == reader);
}

TEST(interrupts_are_left_exactly_as_they_were_found)
{
	pipe_fresh();

	CHECK(fake_task_interrupts_enabled());

	pipe_write(&p, "abc", 3);
	char out[4] = { 0 };
	pipe_read(&p, out, 3);

	/* Every irq_save matched by an irq_restore. An unbalanced pair leaves
	 * interrupts off, and a kernel with interrupts off stops scheduling -
	 * a hang with no message and nothing to see. */
	CHECK_EQ(fake_task_irq_depth(), 0);
	CHECK(fake_task_interrupts_enabled());
}

TEST(stats_tolerate_null_arguments)
{
	pipe_fresh();

	pipe_stats(&p, NULL, NULL);
}

int main(void)
{
	RUN(a_new_pipe_is_empty);
	RUN(bytes_come_out_in_the_order_they_went_in);
	RUN(a_read_returns_what_is_there_rather_than_waiting_for_more);
	RUN(reading_zero_bytes_returns_immediately);
	RUN(writing_zero_bytes_does_nothing);
	RUN(the_ring_wraps_without_losing_or_reordering_bytes);
	RUN(a_full_pipe_holds_exactly_its_capacity);
	RUN(a_writer_blocks_when_the_pipe_is_full_and_resumes_after_a_read);
	RUN(a_reader_blocks_on_an_empty_pipe_until_a_writer_arrives);
	RUN(a_write_wakes_a_waiting_reader);
	RUN(interrupts_are_left_exactly_as_they_were_found);
	RUN(stats_tolerate_null_arguments);

	return test_report("pipe");
}
