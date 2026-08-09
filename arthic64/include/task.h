/* task.h - kernel threads and the scheduler, 64-bit.
 *
 * Same idea as the 32-bit branch: everything so far has been one thread of
 * control, borrowed briefly by interrupts and handed straight back. A
 * scheduler makes several threads exist, each with its own stack, and lets
 * the timer decide which one runs - preemptive, none of them cooperating,
 * none of them aware they were ever interrupted.
 */
#ifndef ARTHIC_TASK_H
#define ARTHIC_TASK_H

#include <stdint.h>

#define TASK_NAME_MAX 16

enum task_state {
	TASK_READY,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_BLOCKED,
	TASK_FINISHED
};

struct task {
	/* MUST be the first field - switch.s writes the saved stack pointer
	 * through a plain pointer to the struct, so its offset has to be zero. */
	uint64_t esp;   /* named esp for continuity with the 32-bit branch's
	                 * comments and tooling; it holds RSP */

	uint32_t     id;
	char         name[TASK_NAME_MAX];
	enum task_state state;
	uint64_t     stack_base;
	uint64_t     stack_frames;
	uint64_t     wake_tick;
	uint64_t     kernel_stack_top;   /* TSS.RSP0 while this task runs */

	/* Address space this task runs in, or 0 for the kernel's own. The
	 * scheduler switches CR3 to this on every task switch where it differs
	 * from the previous task's. */
	uint64_t     page_dir;

	/* Whatever the creator wanted this task to know, and who to tell when it
	 * dies. Both are set before the task joins the run ring, never after -
	 * it can be scheduled the instant it does. */
	void        *arg;
	void       (*on_exit)(void *arg);

	struct task *wait_next;
	struct task *next;
};

void         task_init(void);
uint32_t     task_create(const char *name, void (*entry)(void));

/* Create a task with its address space and argument already in place.
 *
 * Separate from task_create because ordering matters: a task becomes
 * schedulable the instant it joins the run ring, and the timer can fire
 * immediately afterwards. Anything the task needs must be set BEFORE that,
 * not after - task_create alone has no way to express that. */
uint32_t     task_create_ex(const char *name, void (*entry)(void),
                            uint64_t page_dir, void *arg,
                            void (*on_exit)(void *arg));

/* Kill the running task from wherever we are, including from inside a fault
 * handler. Does not return - the current kernel stack is abandoned along
 * with whatever is on it, which is fine because the whole stack is freed when
 * the task is reaped. */
void         task_terminate(void);
void         task_schedule(void);
void         task_exit(void);
void         task_yield(void);
void         task_sleep(uint32_t ticks);
void         task_block(void);
void         task_unblock(struct task *t);
uint32_t     task_switch_count(void);
void         task_list(void);
struct task *task_current(void);
struct task *task_by_id(uint32_t id);

uint64_t irq_save(void);
void     irq_restore(uint64_t flags);

#endif
