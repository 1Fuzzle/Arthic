/* pipe.h - a channel between two tasks.
 *
 * The first thing here that lets separate threads of control cooperate rather
 * than merely coexist. One writes, another reads, and the pipe handles the two
 * cases that make it interesting: a reader arriving before there is anything to
 * read, and a writer arriving when there is no room.
 *
 * Both cases are solved by blocking, which already exists. That is worth
 * noticing - a pipe is not a new mechanism, it is a ring buffer plus the wait
 * queues built for mutexes, pointed at a different question.
 */
#ifndef ARTHIC_PIPE_H
#define ARTHIC_PIPE_H

#include <stdint.h>

#include "waitqueue.h"

#define PIPE_CAPACITY 256

struct pipe {
	char     buffer[PIPE_CAPACITY];
	uint32_t read_pos;
	uint32_t write_pos;
	uint32_t count;

	/* Two queues, one per direction, both the same shared FIFO as a mutex
	 * uses. A reader waits for the pipe to be non-empty and a writer for it
	 * to be non-full, and nothing else about the waiting differs. */
	struct wait_queue readers;
	struct wait_queue writers;

	uint32_t blocked_reads;
	uint32_t blocked_writes;
};

void     pipe_init(struct pipe *p);
uint32_t pipe_write(struct pipe *p, const char *data, uint32_t length);
uint32_t pipe_read(struct pipe *p, char *out, uint32_t max);
uint32_t pipe_available(struct pipe *p);
void     pipe_stats(struct pipe *p, uint32_t *blocked_reads,
                    uint32_t *blocked_writes);

#endif
