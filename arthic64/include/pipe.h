/* pipe.h - a bounded channel between tasks, 64-bit.
 *
 * Straight port. A pipe is a ring buffer plus the wait queues already built
 * for the mutex, pointed at a different question - "is there data" instead of
 * "is the lock free". None of that logic cares about pointer width.
 */
#ifndef ARTHIC_PIPE_H
#define ARTHIC_PIPE_H

#include <stdint.h>

#define PIPE_CAPACITY 256

struct task;

struct pipe {
	char     buffer[PIPE_CAPACITY];
	uint64_t read_pos;
	uint64_t write_pos;
	uint64_t count;

	struct task *readers_head, *readers_tail;
	struct task *writers_head, *writers_tail;

	uint64_t blocked_reads;
	uint64_t blocked_writes;
};

void     pipe_init(struct pipe *p);
uint64_t pipe_write(struct pipe *p, const char *data, uint64_t length);
uint64_t pipe_read(struct pipe *p, char *out, uint64_t max);
uint64_t pipe_available(struct pipe *p);
void     pipe_stats(struct pipe *p, uint64_t *blocked_reads,
                    uint64_t *blocked_writes);

#endif
