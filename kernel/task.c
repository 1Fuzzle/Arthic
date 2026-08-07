/* task.c — kernel threads and a round-robin scheduler.
 *
 * HOW A NEW THREAD STARTS
 *
 * task_switch always resumes a thread by popping registers and executing `ret`.
 * That is fine for a thread that was switched out — its stack holds exactly
 * what is needed. A brand new thread has never run, so there is nothing there.
 *
 * The trick is to FAKE the stack it would have had. Write the values a switch
 * would have pushed, and put the entry point where the return address goes.
 * Then the first switch into it pops zeros into the registers and `ret`s
 * straight into the function. The thread cannot tell it was never running.
 *
 * Below the entry point we also plant task_exit, so a thread whose function
 * simply returns lands somewhere sensible instead of executing whatever bytes
 * happen to be next.
 *
 * WHY THE EOI ORDERING MATTERS
 *
 * task_schedule is called from the timer interrupt, and it may not come back
 * for a long time — the CPU goes off and runs another thread. If the interrupt
 * controller has not been acknowledged before that happens, it waits forever
 * for an acknowledgement that is now stuck behind whatever the other thread is
 * doing, and no timer interrupt ever fires again. The whole system stops.
 *
 * So idt.c sends the EOI first and schedules afterwards. That ordering is not a
 * detail, it is the difference between working and hanging.
 */

#include "task.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "terminal.h"

#define STACK_FRAMES 2   /* 8 KB per thread */

extern void task_switch(uint32_t *save_esp_here, uint32_t new_esp);

static struct task *current  = 0;
static struct task *task_ring = 0;   /* circular list */
static uint32_t     next_id  = 0;
static int          enabled  = 0;

static void copy_name(char *dest, const char *src)
{
	int i = 0;
	while (src[i] && i < TASK_NAME_MAX - 1) {
		dest[i] = src[i];
		i++;
	}
	dest[i] = '\0';
}

void task_init(void)
{
	struct task *t = (struct task *) kmalloc(sizeof(struct task));
	if (!t) {
		kprintf("task: cannot allocate the initial task\n");
		return;
	}

	kmemset(t, 0, sizeof(*t));

	copy_name(t->name, "kernel");
	t->id    = next_id++;
	t->state = TASK_RUNNING;
	t->next  = t;                 /* a ring of one */

	/* No stack to set up: this task is already running on one. Its esp gets
	 * filled in the first time it is switched away from. */

	current   = t;
	task_ring = t;
	enabled   = 1;
}

uint32_t task_create(const char *name, void (*entry)(void))
{
	if (!enabled)
		return 0;

	struct task *t = (struct task *) kmalloc(sizeof(struct task));
	if (!t)
		return 0;

	kmemset(t, 0, sizeof(*t));

	uint32_t stack = pmm_alloc_frames(STACK_FRAMES);
	if (!stack) {
		kfree(t);
		return 0;
	}

	t->stack_base   = stack;
	t->stack_frames = STACK_FRAMES;

	/* Build the stack the switch expects to find. Highest address first,
	 * because stacks grow down. */
	uint32_t *sp = (uint32_t *)(stack + STACK_FRAMES * PAGE_SIZE);

	*(--sp) = (uint32_t) task_exit;   /* where entry returns to */
	*(--sp) = (uint32_t) entry;       /* what `ret` jumps to    */
	*(--sp) = 0x00000202;             /* eflags, interrupt flag set */
	*(--sp) = 0;                      /* ebx */
	*(--sp) = 0;                      /* esi */
	*(--sp) = 0;                      /* edi */
	*(--sp) = 0;                      /* ebp */

	t->esp   = (uint32_t) sp;
	t->id    = next_id++;
	t->state = TASK_READY;
	copy_name(t->name, name);

	/* Splice into the ring, immediately after the current task. Inserting
	 * here rather than at some notional end keeps it O(1) and there is no
	 * meaningful ordering in round robin anyway. */
	__asm__ volatile ("cli");
	t->next       = current->next;
	current->next = t;
	__asm__ volatile ("sti");

	return t->id;
}

/* Reclaim a finished task. Done by whichever task notices it, rather than by
 * the task itself — a thread cannot free the stack it is standing on. */
static void reap(struct task *prev, struct task *dead)
{
	prev->next = dead->next;

	for (uint32_t i = 0; i < dead->stack_frames; i++)
		pmm_free_frame(dead->stack_base + i * PAGE_SIZE);

	kfree(dead);
}

void task_schedule(void)
{
	if (!enabled || !current)
		return;

	struct task *prev = current;
	struct task *next = current->next;

	/* Walk forward for a runnable task, cleaning up dead ones on the way.
	 * Bounded by the ring length so a ring of nothing but corpses cannot spin
	 * forever. */
	uint32_t guard = 0;
	while (next != current && guard++ < 64) {
		if (next->state == TASK_FINISHED) {
			struct task *dead = next;
			next = next->next;
			reap(prev, dead);
			continue;
		}
		break;
	}

	if (next == current || !next)
		return;                       /* nothing else to run */

	if (current->state == TASK_RUNNING)
		current->state = TASK_READY;

	next->state = TASK_RUNNING;
	current     = next;

	task_switch(&prev->esp, next->esp);

	/* Execution resumes here when something switches back to `prev`. Note
	 * that by then `current` points at a different task — the local variables
	 * are on this thread's stack and survived, but the globals have moved on
	 * without us. That mental adjustment is most of what makes scheduler code
	 * confusing to read. */
}

void task_yield(void)
{
	task_schedule();
}

void task_exit(void)
{
	if (current)
		current->state = TASK_FINISHED;

	/* Do not free anything here — we are standing on the stack in question.
	 * Another task reaps us once we are no longer running on it. */
	for (;;)
		task_schedule();
}

void task_list(void)
{
	if (!enabled) {
		kprintf("scheduler not running\n");
		return;
	}

	struct task *t = task_ring;
	uint32_t guard = 0;

	kprintf("  id  state    name\n");

	do {
		const char *state = t->state == TASK_RUNNING  ? "running"
		                  : t->state == TASK_READY    ? "ready"
		                  :                             "finished";

		kprintf("  %u   %s%s  %s\n", t->id, state,
		        t->state == TASK_READY ? "   " : " ", t->name);

		t = t->next;
	} while (t != task_ring && guard++ < 32);
}

struct task *task_current(void)
{
	return current;
}
