/* usermode.c - the first entry to ring 3, and the demo that proves SYSCALL
 * actually works.
 *
 * ENTERING RING 3 STILL USES IRETQ, NOT SYSRET
 *
 * SYSRET is the return half of a pair whose entry half is SYSCALL. The very
 * first drop into ring 3 has no matching entry to return from - there is no
 * "user RIP the CPU remembers" yet, because the user program has never run.
 * So the first transition is built the same way as the 32-bit branch's: fake
 * an interrupt return frame by hand and execute IRETQ. SYSRET only becomes
 * relevant once the program itself calls SYSCALL and needs to get back.
 *
 * WHERE THE DEMO LIVES
 *
 * No ELF loader exists yet on this branch, so the "user program" here is
 * ordinary C linked into the same kernel image, placed in its own section and
 * given ring-3 permissions afterward. That mirrors exactly how the 32-bit
 * branch introduced ring 3 originally, before the loader existed - the ELF
 * work is its own later step, not a prerequisite for this one.
 */

#include "usermode.h"
#include "syscall.h"
#include "paging.h"
#include "pmm.h"
#include "tss.h"
#include "terminal.h"
#include "string.h"

extern uint64_t user_text_start;
extern uint64_t user_text_end;

#define USER_STACK_PAGES 2

static uint64_t user_stack_phys = 0;
static uint64_t saved_rsp = 0, saved_rbp = 0;

/* Where SYS_EXIT (or a fault) resumes. In assembly for the same reason the
 * probe_write resume point is: the target must be an exact instruction
 * address, and the compiler is free to move a C label relative to surrounding
 * code at -O2. */
extern void usermode_return(void);
extern void usermode_jump(uint64_t entry, uint64_t user_stack_top);

/* Two separate bugs lived here during testing, and both are worth keeping the
 * comments for - this is exactly the kind of thing that looks fine, compiles
 * clean, and fails in a way that points nowhere near the actual cause.
 *
 * BUG ONE: no output operand. The asm originally had no way to tell the
 * compiler that SYSCALL changes RAX at all - only that "a"(number) reads it
 * going in. Under optimisation the compiler reused a value left over from a
 * PREVIOUS call's kernel-supplied return code as the syscall number for the
 * next one. The fix is "=a"(ret) below, tying RAX as an output too.
 *
 * BUG TWO, which the fix for bug one did not actually solve: at -O2, GCC
 * inlined every call site into user_program and folded four separate syscalls
 * down to ONE register load followed by four bare `syscall` instructions -
 * because `number` is the same compile-time constant (0, SYS_WRITE) each
 * time, and the compiler's dataflow tracking, seeing the output `ret` go
 * unused at each call site, concluded it could keep treating RAX as still
 * holding that constant across every one of them. The disassembly showed it
 * plainly: one `mov` before four `syscall`s in a row. `volatile` stops the
 * asm from being deleted or reordered; it does not stop the compiler folding
 * multiple textual instances of it together once they have been inlined into
 * each other.
 *
 * `noinline` is the fix, and it is the one real syscall wrappers actually use
 * for exactly this reason: it guarantees ONE compiled copy of this function,
 * reached by a real `call` every time, so there is no second inlined instance
 * for the optimiser to fold this one into. */
__attribute__((section(".usertext"), noinline))
static uint64_t user_syscall(uint64_t number, uint64_t arg)
{
	uint64_t ret;
	__asm__ volatile (
		"syscall"
		: "=a"(ret)
		: "a"(number), "D"(arg)
		: "rcx", "r11", "memory"
	);
	return ret;
}

__attribute__((section(".userdata")))
static const char msg1[] = "  [ring 3] hello via SYSCALL, not int 0x80\n";

__attribute__((section(".userdata")))
static const char msg2[] = "  [ring 3] trying to read kernel memory\n";

__attribute__((section(".userdata")))
static const char msg3[] = "  [ring 3] now writing to my own code segment\n";

__attribute__((section(".usertext")))
void user_program(void)
{
	user_syscall(SYS_WRITE, (uint64_t) msg1);
	user_syscall(SYS_WRITE, (uint64_t) msg2);

	/* Same deliberate misbehaviour as the 32-bit demo: hand the kernel a
	 * pointer into kernel memory and see whether it gets away with it. This
	 * is caught by the SYSCALL argument check, before the pointer is ever
	 * dereferenced - a validation failure, not a fault. */
	user_syscall(SYS_WRITE, 0x100000);

	/* A different failure entirely: this one is not a bad argument, it is a
	 * genuine access violation caught by the MMU rather than by a check we
	 * wrote. The code segment paging_make_user mapped is r-x - present,
	 * executable, not writable. This should page-fault, and the kernel
	 * should terminate the program rather than halt - proving the
	 * user-fault path in mm/paging.c, not just the syscall validation path
	 * exercised above. Nothing after this line runs. */
	user_syscall(SYS_WRITE, (uint64_t) msg3);
	*(volatile char *) user_program = 0;

	for (;;) { }   /* unreachable */
}

void usermode_init(void)
{
	uint64_t text_start = (uint64_t) &user_text_start;
	uint64_t text_end   = (uint64_t) &user_text_end;

	user_stack_phys = pmm_alloc_frames(USER_STACK_PAGES);
	if (!user_stack_phys) {
		kprintf("usermode: no memory for a user stack\n");
		return;
	}
	kmemset((void *) user_stack_phys, 0, USER_STACK_PAGES * PAGE_SIZE);

	/* Code: readable and executable, NOT writable. Stack: writable, and NX
	 * because paging_make_user sets that automatically for anything marked
	 * writable - the actual, unconditional other half of W^X this time. */
	paging_make_user(text_start, text_end, 0);
	paging_make_user(user_stack_phys, user_stack_phys + USER_STACK_PAGES * PAGE_SIZE, 1);

	syscall_set_user_range(text_start, user_stack_phys + USER_STACK_PAGES * PAGE_SIZE);
}

void usermode_run(void)
{
	if (!user_stack_phys) {
		kprintf("usermode: not initialised\n");
		return;
	}

	/* TSS.rsp0 is for genuine interrupts and exceptions arriving while ring 3
	 * runs - the timer, a fault. It is a completely different mechanism from
	 * the syscall scratch stack in syscall.c, which SYSCALL uses instead and
	 * which the CPU never touches on its own. */
	uint64_t kernel_stack;
	__asm__ volatile ("mov %%rsp, %0" : "=r" (kernel_stack));
	tss_set_kernel_stack(kernel_stack - 256);

	uint64_t stack_top = user_stack_phys + USER_STACK_PAGES * PAGE_SIZE - 16;

	usermode_jump((uint64_t) user_program, stack_top);

	/* Control returns here via usermode_return, called from SYS_EXIT or a
	 * fault - see syscall.c and the page fault handler. */
}
