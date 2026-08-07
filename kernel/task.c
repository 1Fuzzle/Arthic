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
#include "timer.h"

#define STACK_FRAMES 2   /* 8 KB per thread */

extern void task_switch(uint32_t *save_esp_here, uint32_t new_esp);

static struct task *current  = 0;
static struct task *task_ring = 0;   /* circular list */
static uint32_t     next_id  = 0;
static int          enabled  = 0;
static uint32_t     switches = 0;

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

/* Move any sleeper whose time has come back to READY.
 *
 * A linear pass over every task on every tick. With thousands of tasks you
 * would keep them in a queue ordered by wake time and look at only the head;
 * with a handful, walking the ring is faster than maintaining the queue. Worth
 * knowing which one you are choosing and why. */
static void wake_sleepers(void)
{
	uint32_t now = timer_get_ticks();
	struct task *t = task_ring;
	uint32_t guard = 0;

	do {
		if (t->state == TASK_SLEEPING && now >= t->wake_tick)
			t->state = TASK_READY;
		t = t->next;
	} while (t != task_ring && guard++ < 64);
}

void task_schedule(void)
{
	if (!enabled || !current)
		return;

	/* Interrupts off across the switch. This function is reached both from
	 * the timer interrupt (where they are already off) and from a thread
	 * calling sleep or yield (where they are on), and a timer tick landing
	 * halfway through would try to schedule inside scheduling.
	 *
	 * `flags` is a local, so it lives on this thread's stack and is still
	 * correct whenever this thread is resumed - possibly much later. */
	uint32_t flags = irq_save();

	wake_sleepers();

	struct task *prev = current;

	/* Walk the ring for something runnable, reaping corpses on the way. */
	struct task *scan = current;
	struct task *next = 0;
	uint32_t guard = 0;

	while (guard++ < 64) {
		struct task *candidate = scan->next;

		if (candidate == current)
			break;                     /* full lap, nothing else ready */

		if (candidate->state == TASK_FINISHED) {
			reap(scan, candidate);     /* scan->next now points past it */
			continue;
		}

		if (candidate->state == TASK_READY) {
			next = candidate;
			break;
		}

		scan = candidate;              /* sleeping - skip it entirely */
	}

	if (!next) {
		/* Nothing else can run. If we are still runnable, carry on. If not -
		 * we just slept or blocked - fall back to task 0, which never does
		 * either. That is the idle task in all but name. */
		if (current->state == TASK_RUNNING) {
			irq_restore(flags);
			return;
		}
		next = task_ring;
	}

	if (current->state == TASK_RUNNING)
		current->state = TASK_READY;

	next->state = TASK_RUNNING;
	current     = next;
	switches++;

	task_switch(&prev->esp, next->esp);

	/* Resumed. Restore the interrupt state this thread had when it left. */
	irq_restore(flags);

	/* Execution resumes here when something switches back to `prev`. Note
	 * that by then `current` points at a different task — the local variables
	 * are on this thread's stack and survived, but the globals have moved on
	 * without us. That mental adjustment is most of what makes scheduler code
	 * confusing to read. */
}

uint32_t irq_save(void)
{
	uint32_t flags;
	__asm__ volatile ("pushfl; popl %0; cli" : "=r" (flags) :: "memory");
	return flags;
}

void irq_restore(uint32_t flags)
{
	if (flags & 0x200)
		__asm__ volatile ("sti" ::: "memory");
}

void task_block(void)
{
	if (!enabled || !current)
		return;

	/* Task 0 is the fallback when nothing else can run, so it must never
	 * become unrunnable. If it would block, spin instead - correct, just
	 * wasteful, and it keeps the invariant that something is always
	 * schedulable. */
	if (current == task_ring) {
		task_yield();
		return;
	}

	current->state = TASK_BLOCKED;
	task_schedule();
}

void task_unblock(struct task *t)
{
	if (t && t->state == TASK_BLOCKED)
		t->state = TASK_READY;
}

void task_yield(void)
{
	task_schedule();
}

void task_sleep(uint32_t ticks)
{
	if (!enabled || !current || current == task_ring) {
		/* Task 0 must never sleep - it is the fallback when nothing else can
		 * run. Busy-wait instead. */
		uint32_t target = timer_get_ticks() + ticks;
		while (timer_get_ticks() < target)
			__asm__ volatile ("hlt");
		return;
	}

	current->wake_tick = timer_get_ticks() + ticks;
	current->state     = TASK_SLEEPING;

	task_schedule();
}

uint32_t task_switch_count(void)
{
	return switches;
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

	kprintf("  %u context switches so far\n", switches);
	kprintf("  id  state     name\n");

	do {
		const char *state = t->state == TASK_RUNNING  ? "running "
		                  : t->state == TASK_READY    ? "ready   "
		                  : t->state == TASK_SLEEPING ? "sleeping"
		                  : t->state == TASK_BLOCKED  ? "blocked "
		                  :                             "finished";

		kprintf("  %u   %s  %s\n", t->id, state, t->name);

		t = t->next;
	} while (t != task_ring && guard++ < 32);
}

struct task *task_current(void)
{
	return current;
}
