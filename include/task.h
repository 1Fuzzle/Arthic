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
	TASK_SLEEPING,   /* not runnable until wake_tick        */
	TASK_BLOCKED,    /* not runnable until someone wakes it */
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

	/* Top of this task's KERNEL stack. The TSS must point here whenever this
	 * task is running, or an interrupt arriving while it is in ring 3 lands
	 * on somebody else's stack. */
	uint32_t     kernel_stack_top;
	uint32_t     wake_tick;      /* meaningful only while TASK_SLEEPING */

	/* Physical address of this task's page directory, or 0 to use the
	 * kernel's. Switching tasks now means switching what addresses MEAN. */
	uint32_t     page_dir;

	/* Where to resume if this task drops to ring 3 and comes back. Per-task
	 * rather than global, because two tasks can each be in ring 3 with only
	 * one of them currently running. */
	uint32_t     uctx[2];
	/* Whatever the creator wanted this task to know. Set before the task is
	 * made schedulable, so it is always valid by the time the task runs. */
	void        *arg;

	/* Called with `arg` once the task is dead and off the run queue. Cleanup
	 * belongs here rather than at the end of the task's own function: a task
	 * cannot free the stack it is standing on, and a task that faulted never
	 * reaches the end of its function at all. */
	void       (*on_exit)(void *arg);

	struct task *next;

	/* Link field for whatever wait queue this task is parked on. Intrusive
	 * lists like this are everywhere in kernels: no allocation is needed to
	 * put a task on a queue, which matters when the code doing it may be
	 * holding a lock or running with interrupts off. */
	struct task *wait_next;
};

/* Turn the current thread of control into task 0. Everything running before
 * this becomes a schedulable task rather than "the kernel". */
void task_init(void);

/* Create a task that begins at `entry`. Returns its id, or 0 on failure. */
uint32_t task_create(const char *name, void (*entry)(void));

/* Create a task with its address space and argument already in place.
 *
 * Separate from task_create because the ordering matters: a task becomes
 * schedulable the moment it joins the ring, and the timer can fire immediately
 * afterwards. Anything the task needs must be set BEFORE that, not after. */
uint32_t task_create_ex(const char *name, void (*entry)(void),
                        uint32_t page_dir, void *arg,
                        void (*on_exit)(void *arg));

/* Kill the running task from wherever we are - including from inside an
 * interrupt handler. Does not return. The current kernel stack is abandoned
 * along with whatever is on it, which is fine because the whole stack is freed
 * when the task is reaped. */
void task_terminate(void);

/* Give the running task its own address space. Passing 0 puts it back on the
 * kernel's. */
void task_set_address_space(uint32_t page_dir_phys);

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

/* Stop running until somebody calls task_unblock. Unlike sleeping there is no
 * timeout - if nobody wakes it, it waits forever. */
void task_block(void);
void task_unblock(struct task *t);

/* Save the interrupt flag and disable interrupts; restore it later. Returned
 * value must be treated as opaque. */
uint32_t irq_save(void);
void     irq_restore(uint32_t flags);

/* How many context switches have happened. Cheap way to see the scheduler
 * doing work. */
uint32_t task_switch_count(void);

void         task_list(void);
struct task *task_current(void);
struct task *task_by_id(uint32_t id);

#endif
