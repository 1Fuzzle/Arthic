/* fake_task.c - a scheduler-shaped hole, filled in.
 *
 * pipe.c and lock.c both do the same thing when they cannot proceed: turn
 * interrupts off, join a wait queue, and call task_block() to stop being
 * runnable. In the kernel, task_block switches to another task and only
 * returns once somebody calls task_unblock. There is no other task here, so a
 * faithful fake would simply hang.
 *
 * Instead task_block runs a hook the test installed. The hook plays the part
 * of whoever else was going to run - draining the pipe, releasing the mutex -
 * and then task_block returns, exactly as it would after the real scheduler
 * came back round. Every path that only happens under contention becomes
 * reachable from a single-threaded test, and deterministically so, which is
 * more than can be said for the real thing.
 *
 * irq_save/irq_restore keep a counter rather than touching EFLAGS. `cli` is
 * privileged; a user process that executes it is killed. Counting instead also
 * gives the tests something the kernel cannot easily check - that every
 * disable is matched by a restore, and that the code under test really was
 * holding interrupts off when it queued itself.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "support.h"
#include "task.h"

#define FAKE_TASK_MAX 8

static struct task  tasks[FAKE_TASK_MAX];
static uint32_t     task_count;
static struct task *current;

static uint32_t block_count;
static uint32_t unblock_count;
static struct task *last_unblocked;

static int      interrupts_enabled = 1;
static uint32_t irq_depth;

static void (*block_hook)(void *arg);
static void  *block_hook_arg;

void fake_task_reset(void)
{
	memset(tasks, 0, sizeof(tasks));
	task_count = 0;

	block_count    = 0;
	unblock_count  = 0;
	last_unblocked = NULL;

	interrupts_enabled = 1;
	irq_depth = 0;

	block_hook     = NULL;
	block_hook_arg = NULL;

	current = fake_task_make("test");
}

struct task *fake_task_make(const char *name)
{
	if (task_count >= FAKE_TASK_MAX)
		abort();

	struct task *t = &tasks[task_count];

	memset(t, 0, sizeof(*t));
	t->id = ++task_count;
	t->state = TASK_RUNNING;
	strncpy(t->name, name, TASK_NAME_MAX - 1);

	return t;
}

void fake_task_set_current(struct task *t) { current = t; }

void fake_task_on_block(void (*hook)(void *arg), void *arg)
{
	block_hook     = hook;
	block_hook_arg = arg;
}

uint32_t     fake_task_block_count(void)   { return block_count; }
uint32_t     fake_task_unblock_count(void) { return unblock_count; }
struct task *fake_task_last_unblocked(void){ return last_unblocked; }
int fake_task_interrupts_enabled(void)     { return interrupts_enabled; }
uint32_t fake_task_irq_depth(void)         { return irq_depth; }

struct task *task_current(void)
{
	return current;
}

void task_block(void)
{
	block_count++;
	current->state = TASK_BLOCKED;

	/* The real task_block switches away with interrupts off and the scheduler
	 * restores them on the other side. Anything the hook does is therefore
	 * standing in for a different task running with its own flags, so the
	 * fake pretends interrupts came back on for the duration. */
	int saved = interrupts_enabled;
	interrupts_enabled = 1;

	if (block_hook)
		block_hook(block_hook_arg);

	interrupts_enabled = saved;
	current->state = TASK_RUNNING;
}

void task_unblock(struct task *t)
{
	unblock_count++;
	last_unblocked = t;

	if (t->state == TASK_BLOCKED)
		t->state = TASK_READY;
}

uint32_t irq_save(void)
{
	uint32_t was_enabled = interrupts_enabled ? 1u : 0u;

	interrupts_enabled = 0;
	irq_depth++;

	return was_enabled;
}

void irq_restore(uint32_t flags)
{
	if (irq_depth > 0)
		irq_depth--;

	if (flags)
		interrupts_enabled = 1;
}
