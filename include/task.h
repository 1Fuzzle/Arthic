/* task.h — kernel threads and the scheduler.
 *
 * Everything so far has been one thread of control: kernel_main runs, calls
 * things, and eventually idles. Interrupts borrow the CPU briefly and give it
 * straight back.
 *
 * A scheduler changes that. Several threads exist, each with its own stack and
 * its own saved registers, and the timer decides which one runs. None of them
 * knows it is being interrupted, and none of them cooperates — that is what
 * makes it PREEMPTIVE, and it is the difference between an OS and a program
 * with a main loop.
 */
#ifndef ARTHIC_TASK_H
#define ARTHIC_TASK_H

#include <stdint.h>

#define TASK_NAME_MAX 16

enum task_state {
	TASK_READY,
	TASK_RUNNING,
	TASK_SLEEPING,   /* not runnable until wake_tick */
	TASK_FINISHED
};

struct task {
	/* MUST be the first field. The assembly switch writes the saved stack
	 * pointer through a plain pointer to the struct, so its offset has to be
	 * zero. Move it and the switch silently corrupts whatever is now first. */
	uint32_t esp;

	uint32_t     id;
	char         name[TASK_NAME_MAX];
	enum task_state state;
	uint32_t     stack_base;
	uint32_t     stack_frames;
	uint32_t     wake_tick;      /* meaningful only while TASK_SLEEPING */
	struct task *next;
};

/* Turn the current thread of control into task 0. Everything running before
 * this becomes a schedulable task rather than "the kernel". */
void task_init(void);

/* Create a task that begins at `entry`. Returns its id, or 0 on failure. */
uint32_t task_create(const char *name, void (*entry)(void));

/* Pick the next ready task and switch to it. Called from the timer IRQ, after
 * the interrupt has been acknowledged. */
void task_schedule(void);

/* End the running task. Never returns. */
void task_exit(void);

/* Give up the rest of this time slice voluntarily. */
void task_yield(void);

/* Stop running for at least `ticks` timer ticks. The difference from yielding
 * in a loop is that a sleeping task is not considered for scheduling at all -
 * it costs nothing until it is due. */
void task_sleep(uint32_t ticks);

/* How many context switches have happened. Cheap way to see the scheduler
 * doing work. */
uint32_t task_switch_count(void);

void         task_list(void);
struct task *task_current(void);

#endif
