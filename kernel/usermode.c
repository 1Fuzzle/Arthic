/* usermode.c — dropping into ring 3 and getting back.
 *
 * HOW YOU ENTER RING 3
 *
 * There is no instruction for "lower my privilege". The only way down is to
 * pretend you are returning from an interrupt that came from ring 3 in the
 * first place. You build the stack frame `iret` expects, and execute it.
 *
 * The frame, pushed in this order (so it pops in reverse):
 *
 *     SS      user data selector, RPL 3
 *     ESP     top of the user stack
 *     EFLAGS  with the interrupt flag set, or interrupts stay off forever
 *     CS      user code selector, RPL 3
 *     EIP     where to start executing
 *
 * When `iret` sees a CS whose privilege level is lower than the current one, it
 * performs a full privilege transition: switches stacks, drops to ring 3, and
 * jumps. From the CPU's point of view nothing unusual happened.
 *
 * GETTING BACK
 *
 * Coming back is harder, because ring 3 cannot simply return — there is no
 * instruction to raise privilege either. It has to ASK, via a syscall, and the
 * kernel decides what happens next.
 *
 * With a scheduler this is where you would switch to another task. We have no
 * scheduler, so SYS_EXIT restores the kernel stack pointer we saved on the way
 * in and jumps back to where we left off. Crude, but it is genuinely what a
 * context switch does, minus the bookkeeping for more than one task.
 */

#include "usermode.h"
#include "syscall.h"
#include "paging.h"
#include "pmm.h"
#include "gdt.h"
#include "tss.h"
#include "terminal.h"
#include "task.h"

/* From linker.ld — the section holding code ring 3 may execute. */
extern uint32_t user_text_start;
extern uint32_t user_text_end;

#define USER_STACK_FRAMES 2   /* 8 KB */

static uint32_t user_stack_base = 0;
static uint32_t user_stack_top  = 0;

/* Implemented in interrupts.s. usermode_jump does not return; control comes
 * back through usermode_return, which makes it look as though it did. */
extern void usermode_jump(uint32_t entry, uint32_t user_stack_top);
extern void usermode_return(void);

/* ---- The ring 3 program ---------------------------------------------------
 *
 * `section(".usertext")` puts this in the region we mark user-accessible. It is
 * ordinary C, compiled the same way as everything else — what makes it "user
 * code" is nothing about the code itself, only which page it lives on and what
 * permissions that page carries. Privilege is a property of memory here, not of
 * the instructions.
 *
 * Note what it CANNOT do: no kprintf, because that touches VGA memory which is
 * supervisor-only. No outb, because the TSS I/O bitmap denies port access. Its
 * entire vocabulary is `int $0x80`.
 */
__attribute__((section(".usertext")))
static int user_syscall(int number, int arg)
{
	int result;
	__asm__ volatile ("int $0x80"
	                  : "=a" (result)
	                  : "a" (number), "b" (arg)
	                  : "memory");
	return result;
}

/* Strings go in a separate section from code: the compiler refuses to put
 * data and instructions in one section, since they need different attributes.
 * linker.ld places .userdata immediately after .usertext so both land in the
 * same user-accessible pages. */
__attribute__((section(".userdata")))
static const char user_message[] =
	"  [ring 3] hello from user mode\n";

__attribute__((section(".userdata")))
static const char user_message2[] =
	"  [ring 3] asking the kernel for the tick count\n";

__attribute__((section(".userdata")))
static const char user_message3[] =
	"  [ring 3] now trying to read kernel memory directly\n";

__attribute__((section(".usertext")))
void user_program(void)
{
	user_syscall(SYS_WRITE, (int)(uint32_t) user_message);
	user_syscall(SYS_WRITE, (int)(uint32_t) user_message2);

	int ticks = user_syscall(SYS_TICKS, 0);
	(void) ticks;

	user_syscall(SYS_WRITE, (int)(uint32_t) user_message3);

	/* Deliberately hand the kernel a pointer into kernel memory. A syscall
	 * that trusted its arguments would read it out for us. Ours checks the
	 * range and refuses — you should see the rejection message. */
	user_syscall(SYS_WRITE, 0x100000);

	user_syscall(SYS_EXIT, 0);

	/* Unreachable, but ring 3 falling off the end of a function would be a
	 * fault with nowhere sensible to go. */
	for (;;) { }
}

/* ---- Setup ---------------------------------------------------------------- */

int usermode_init(void)
{
	uint32_t text_start = (uint32_t) &user_text_start;
	uint32_t text_end   = (uint32_t) &user_text_end;

	uint32_t stack_base = pmm_alloc_frames(USER_STACK_FRAMES);
	if (!stack_base)
		return 0;

	uint32_t stack_top = stack_base + USER_STACK_FRAMES * PAGE_SIZE;

	/* Code: readable and executable by ring 3, but NOT writable.
	 * Stack: writable, and we would mark it non-executable if 32-bit paging
	 * had a bit for that. It does not. Noted in the README.
	 *
	 * Both results are checked. If a page in either range turned out not to be
	 * mapped, ring 3 would fault the instant it touched it, and the useful
	 * message is this one - not a page fault report from three layers away. */
	if (!paging_make_user(text_start, text_end, 0) ||
	    !paging_make_user(stack_base, stack_top, 1)) {
		for (uint32_t i = 0; i < USER_STACK_FRAMES; i++)
			pmm_free_frame(stack_base + i * PAGE_SIZE);
		return 0;
	}

	/* Published only once everything above succeeded, so a failed setup leaves
	 * user_stack_top at 0 and usermode_run refuses rather than jumping to
	 * ring 3 with a half-built stack. */
	user_stack_base = stack_base;
	user_stack_top  = stack_top;

	/* The only addresses ring 3 may pass back to the kernel. */
	syscall_set_user_range(text_start, user_stack_top);

	tss_set_kernel_stack(0);   /* filled in properly on entry */

	return 1;
}

void usermode_exit(void)
{
	usermode_return();   /* does not come back */
}

void usermode_run(void)
{
	if (!user_stack_top) {
		kprintf("usermode: not initialised\n");
		return;
	}

	/* The kernel stack the CPU switches to when ring 3 interrupts. It must not
	 * overlap the frame we are standing on, hence the headroom. */
	uint32_t kernel_stack;
	__asm__ volatile ("mov %%esp, %0" : "=r" (kernel_stack));
	tss_set_kernel_stack(kernel_stack - 256);

	usermode_jump((uint32_t) user_program, user_stack_top - 16);
}
