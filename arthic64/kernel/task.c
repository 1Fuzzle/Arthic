/* task.c - kernel threads and a round-robin scheduler, 64-bit.
 *
 * Ported straight from the 32-bit branch; the algorithm does not care about
 * register width. What is genuinely different, and worth reading closely, is
 * how a new thread's first stack frame is faked, and how the TSS interacts
 * with a running scheduler now that SYSCALL exists alongside interrupts.
 *
 * FAKING A NEW THREAD'S FIRST RUN
 *
 * task_switch always resumes by popping seven values and executing `ret`.
 * That is correct for a thread that has run before - its stack holds exactly
 * what a previous switch put there. A brand new thread has never run, so
 * there is nothing on its stack yet. The fix is to write what a switch WOULD
 * have pushed by hand: return address at the top (task_entry_trampoline),
 * then six callee-saved slots and a flags word beneath it. The first switch
 * into it pops plausible-looking garbage into rbx/rbp/r12-r15/flags and
 * `ret`s straight into the trampoline, unable to tell it was never running.
 *
 * WHY A TRAMPOLINE AND NOT THE ENTRY FUNCTION DIRECTLY
 *
 * `ret` needs an address to jump to; it cannot also hand that address an
 * argument the way a `call` would. So the trampoline is what actually runs
 * first - a few instructions that pick the entry point up from a register the
 * stack setup put it in, then jumps for real. It also means a thread whose
 * function simply returns lands somewhere sensible (task_exit) instead of
 * running off into whatever bytes follow.
 *
 * TWO SEPARATE KERNEL STACKS, PER TASK
 *
 * Interrupts and exceptions arriving while a task runs use TSS.RSP0, switched
 * by the scheduler on every task switch, same idea as the 32-bit branch.
 * SYSCALL does not consult the TSS at all - it uses the dedicated scratch
 * stack behind swapgs in kernel/syscall.c, which for now is shared by every
 * task rather than per-task. That is fine as long as only one task is ever
 * inside a syscall handler at once, which is true on a single core with
 * interrupts off across the syscall entry - worth revisiting the moment
 * either of those stops being true.
 */

#include "task.h"
#include "pmm.h"
#include "kheap.h"
#include "string.h"
#include "terminal.h"
#include "timer.h"
#include "tss.h"
#include "paging.h"

#define STACK_FRAMES 2   /* 8 KB per thread */

extern void task_switch(uint64_t *save_esp_here, uint64_t new_esp);
extern void task_entry_trampoline(void);

static struct task *current   = 0;
static struct task *task_ring = 0;
static uint32_t     next_id   = 0;
static int          enabled   = 0;
static uint32_t     switches  = 0;

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
	t->next  = t;

	uint64_t rsp_now;
	__asm__ volatile ("mov %%rsp, %0" : "=r" (rsp_now));
	t->kernel_stack_top = rsp_now - 512;

	current   = t;
	task_ring = t;
	enabled   = 1;
}

uint32_t task_create(const char *name, void (*entry)(void))
{
	return task_create_ex(name, entry, 0, 0, 0);
}

uint32_t task_create_ex(const char *name, void (*entry)(void),
                        uint64_t page_dir, void *arg, void (*on_exit)(void *arg))
{
	if (!enabled)
		return 0;

	struct task *t = (struct task *) kmalloc(sizeof(struct task));
	if (!t)
		return 0;

	kmemset(t, 0, sizeof(*t));

	/* One contiguous run of STACK_FRAMES + 1 frames. The guard has to be
	 * genuinely adjacent to the usable stack - allocating it separately
	 * would give no guarantee it lands next to anything, and an overflow
	 * that misses it entirely defeats the whole point. The LOWEST frame of
	 * the run becomes the guard, since the stack grows down toward it.
	 *
	 * Since every address space shares the SAME identity-mapped physical
	 * RAM - that sharing is exactly what lets a CR3 switch be safe at all -
	 * unmapping a physical address here removes it for every task, not just
	 * this one. That is fine, and is in fact the point: the guard frame is
	 * ALLOCATED (marked used in the PMM bitmap, so nothing else can be
	 * handed the same physical memory) but never mapped anywhere while it
	 * serves as a guard. It is restored to an ordinary mapped page in reap(),
	 * before the frame is freed - skipping that step would leave a stale
	 * unmapped hole that the next thing to reuse this physical memory would
	 * silently inherit, faulting for a reason having nothing to do with it. */
	uint64_t region = pmm_alloc_frames(STACK_FRAMES + 1);
	if (!region) {
		kfree(t);
		return 0;
	}

	uint64_t guard = region;
	uint64_t stack = region + PAGE_SIZE;

	paging_unmap(guard);

	t->guard_phys   = guard;
	t->stack_base   = stack;
	t->stack_frames = STACK_FRAMES;

	/* Highest address first - stacks grow down. This is the fake frame
	 * task_switch expects: the trampoline's address where a return address
	 * belongs, then the six callee-saved slots and flags task_switch will
	 * pop, in the exact order it pops them. */
	uint64_t *sp = (uint64_t *)(stack + STACK_FRAMES * PAGE_SIZE);

	*(--sp) = (uint64_t) task_entry_trampoline;
	*(--sp) = 0x202;         /* rflags - IF set, nothing else */
	*(--sp) = 0;             /* rbx */
	*(--sp) = 0;             /* rbp */
	*(--sp) = 0;             /* r12 */
	*(--sp) = 0;             /* r13 */
	*(--sp) = 0;             /* r14 */
	*(--sp) = (uint64_t) entry;   /* r15 - see the note in switch.s: the
	                               * trampoline reads the entry point back out
	                               * of this exact register, per task, on its
	                               * own stack. A shared global here was the
	                               * actual bug: creating two tasks with
	                               * DIFFERENT entry functions before either
	                               * had run left one global holding only the
	                               * LAST value written, and the first task to
	                               * actually execute jumped into the wrong
	                               * function entirely. `spawn`, which always
	                               * reuses one entry function for every task,
	                               * could never have exposed this - both
	                               * "wrong" values would have been identical.
	                               * Carrying it in a per-task saved register
	                               * instead means there is nothing to share
	                               * and nothing to race. */

	t->esp              = (uint64_t) sp;
	t->kernel_stack_top  = t->esp;   /* refined properly once it first runs;
	                                  * placeholder so it is never zero */
	t->id       = next_id++;
	t->state    = TASK_READY;
	t->page_dir = page_dir;
	t->arg      = arg;
	t->on_exit  = on_exit;
	copy_name(t->name, name);

	__asm__ volatile ("cli");
	t->next       = current->next;
	current->next = t;
	__asm__ volatile ("sti");

	return t->id;
}

static void reap(struct task *prev, struct task *dead)
{
	prev->next = dead->next;

	/* Off the run queue, so it can never be scheduled again - only now is it
	 * safe to release what it owned. */
	if (dead->on_exit)
		dead->on_exit(dead->arg);

	for (uint64_t i = 0; i < dead->stack_frames; i++)
		pmm_free_frame(dead->stack_base + i * PAGE_SIZE);

	if (dead->guard_phys) {
		/* Put the identity mapping back before this frame goes anywhere
		 * near the free list. A future allocation of this same physical
		 * memory expects it to work like ordinary RAM - leaving it unmapped
		 * would make the NEXT thing to receive it fault immediately, for a
		 * reason that has nothing to do with anything it did. */
		paging_map(dead->guard_phys, dead->guard_phys,
		          PAGE_PRESENT | PAGE_WRITE);
		pmm_free_frame(dead->guard_phys);
	}

	kfree(dead);
}

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

uint64_t irq_save(void)
{
	uint64_t flags;
	__asm__ volatile ("pushfq; popq %0; cli" : "=r" (flags) :: "memory");
	return flags;
}

void irq_restore(uint64_t flags)
{
	if (flags & 0x200)
		__asm__ volatile ("sti" ::: "memory");
}

void task_schedule(void)
{
	if (!enabled || !current)
		return;

	uint64_t flags = irq_save();

	wake_sleepers();

	struct task *prev = current;
	struct task *scan = current;
	struct task *next = 0;
	uint32_t guard = 0;

	while (guard++ < 64) {
		struct task *candidate = scan->next;

		if (candidate == current)
			break;

		if (candidate->state == TASK_FINISHED) {
			reap(scan, candidate);
			continue;
		}

		if (candidate->state == TASK_READY) {
			next = candidate;
			break;
		}

		scan = candidate;
	}

	if (!next) {
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

	if (next->kernel_stack_top)
		tss_set_kernel_stack(next->kernel_stack_top);

	/* Switch the address space before the stack - safe only because every
	 * address space contains an identical copy of the kernel's own mappings,
	 * so the code executing right now and the stacks on both sides of the
	 * switch live at addresses that mean the same thing in either PML4. */
	if (next->page_dir != prev->page_dir)
		paging_switch(next->page_dir);

	task_switch(&prev->esp, next->esp);

	irq_restore(flags);
}

void task_yield(void)
{
	task_schedule();
}

void task_sleep(uint32_t ticks)
{
	if (!enabled || !current || current == task_ring) {
		uint32_t target = timer_get_ticks() + ticks;
		while (timer_get_ticks() < target)
			__asm__ volatile ("hlt");
		return;
	}

	current->wake_tick = timer_get_ticks() + ticks;
	current->state     = TASK_SLEEPING;

	task_schedule();
}

void task_block(void)
{
	if (!enabled || !current)
		return;

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

void task_terminate(void)
{
	if (current)
		current->state = TASK_FINISHED;

	/* Switch away and never come back. Whatever was on this kernel stack -
	 * including any interrupt frame that brought us here - is abandoned, and
	 * freed wholesale when the task is reaped. */
	for (;;)
		task_schedule();
}

void task_exit(void)
{
	task_terminate();
}

uint32_t task_switch_count(void)
{
	return switches;
}

struct task *task_by_id(uint32_t id)
{
	struct task *t = task_ring;
	uint32_t guard = 0;

	do {
		if (t->id == id)
			return t;
		t = t->next;
	} while (t != task_ring && guard++ < 64);

	return 0;
}

struct task *task_current(void)
{
	return current;
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
