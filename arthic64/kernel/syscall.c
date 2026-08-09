/* syscall.c - handling requests that arrive via the SYSCALL instruction.
 *
 * HOW EXECUTION GETS HERE
 *
 * Ring 3 code runs `syscall`. The CPU, using the STAR MSR, switches CS/SS to
 * kernel selectors and jumps to the address in LSTAR - no IDT lookup, no gate,
 * no privilege check beyond what SYSCALL itself guarantees. It does this
 * WITHOUT switching stacks and WITHOUT saving anything except RIP (into RCX)
 * and RFLAGS (into R11). Everything else is the kernel's problem, starting
 * the instant control arrives at kernel/syscall_entry.s.
 *
 * WHY GS EXISTS FOR THIS
 *
 * At the moment `syscall` lands, every general-purpose register still holds
 * whatever the user program was doing with it - there is nothing free to hold
 * "where is my kernel stack". SWAPGS solves exactly this: it exchanges the
 * active GS base with a value parked in the KERNEL_GS_BASE MSR, ahead of time,
 * for precisely this moment. After swapgs, %gs: addressing reaches a small
 * fixed structure - here, one stack pointer and one scratch slot - without
 * touching a single general-purpose register. On a multi-core system each
 * core would have its own; on our one core it is simpler, but the mechanism is
 * the same, and staying with it now means the scheduler will not need to
 * change it later.
 *
 * WHAT MUST SURVIVE THE C CALL, AND WHY IT IS NOT OBVIOUS
 *
 * RCX and R11 hold the user's RIP and RFLAGS - the ONLY place the CPU put
 * them. As far as the C ABI is concerned, both are ordinary caller-saved
 * registers, free for any function to clobber. An unremarkable, correctly
 * compiled C function is entitled to use RCX or R11 as scratch space and will
 * often do so. If the assembly stub does not save them before calling C and
 * restore them immediately before SYSRET, the return address SYSRET uses is
 * whatever the compiler last left in RCX - not a crash exactly, but the
 * program resumes somewhere neither it nor the kernel chose. This has no
 * analogue on the 32-bit branch: IRET pulls the return address out of the
 * exception frame on the stack, not out of a general-purpose register a
 * compiler is free to reuse.
 */

#include "syscall.h"
#include "terminal.h"
#include "task.h"
#include "string.h"

extern void usermode_return(void);

extern void syscall_entry(void);

/* The two slots swapgs makes reachable via %gs:0 and %gs:8. Field order and
 * size must match what syscall_entry.s addresses directly - there is no
 * compiler-checked link between them, only this comment. */
struct cpu_data {
	uint64_t kernel_rsp;   /* %gs:0  - top of the syscall scratch stack */
	uint64_t user_rsp;     /* %gs:8  - the caller's stack, parked here  */
};

static struct cpu_data cpu0;

/* A small dedicated stack purely for running syscall_dispatch. Distinct from
 * TSS.rsp0: that one is for interrupts arriving from ring 3, which the CPU
 * switches to on its own. This one exists because SYSCALL switches nothing,
 * and the kernel has to have somewhere of its own to stand. */
static uint8_t syscall_stack[4096] __attribute__((aligned(16)));

static uint64_t user_range_start = 0;
static uint64_t user_range_end   = 0;

void syscall_set_user_range(uint64_t start, uint64_t end)
{
	user_range_start = start;
	user_range_end   = end;
}

/* Same three checks as the 32-bit pipe write, and for the same reason: start
 * in range, length sane on its own, end in range. Skipping the middle one lets
 * ptr + length overflow past zero and pass the final check regardless. */
static int user_string_ok(uint64_t ptr, uint64_t max_length)
{
	if (ptr < user_range_start || ptr >= user_range_end)
		return 0;

	for (uint64_t i = 0; i < max_length; i++) {
		uint64_t addr = ptr + i;

		if (addr >= user_range_end)
			return 0;

		if (*(const char *) addr == '\0')
			return 1;
	}

	return 0;
}

/* Called from syscall_entry.s with a pointer to the frame it built. Return by
 * writing frame->rax - that is what ends up back in the user program's RAX. */
void syscall_dispatch(struct syscall_frame *f)
{
	switch (f->rax) {

	case SYS_WRITE:
		if (!user_string_ok(f->rdi, 256)) {
			kprintf("[syscall] rejected bad string pointer 0x%lx\n", f->rdi);
			f->rax = (uint64_t) -1;
			return;
		}
		terminal_write((const char *) f->rdi);
		f->rax = 0;
		return;

	case SYS_EXIT: {
		/* A syscall that merely returns brings the program straight back to
		 * the instruction after `syscall` - which is exactly what happened
		 * the first time this was written: SYS_EXIT "succeeded" and the demo
		 * walked straight into its own infinite loop, in ring 3, forever.
		 *
		 * Two different ways to leave ring 3 for good now exist, and which
		 * one applies depends on WHAT is running. A loaded program (loader.c)
		 * is its own task and dies as one, via task_terminate - the same path
		 * a fault takes. The built-in demo in usermode.c runs on the shell's
		 * OWN task, which must survive, so it uses the older IRETQ-reversed
		 * unwind instead. on_exit is set only for loaded programs, which is
		 * what makes it the right thing to branch on here. */
		struct task *t = task_current();

		if (t && t->on_exit)
			task_terminate();

		usermode_return();
		return;   /* unreachable */
	}

	default:
		kprintf("[syscall] unknown call %lu\n", f->rax);
		f->rax = (uint64_t) -1;
		return;
	}
}

void syscall_install(void)
{
	cpu0.kernel_rsp = (uint64_t) syscall_stack + sizeof(syscall_stack);
	cpu0.user_rsp   = 0;

	/* KERNEL_GS_BASE (0xC0000102) is the value SWAPGS exchanges in. GS_BASE
	 * itself (0xC0000101) is left at 0 - that is "the user's meaning of GS",
	 * unused by our tiny demo program but present for correctness. */
	uint32_t low, high;

	low  = (uint32_t)((uint64_t) &cpu0 & 0xFFFFFFFFu);
	high = (uint32_t)((uint64_t) &cpu0 >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000102u));

	__asm__ volatile ("wrmsr" : : "a"(0), "d"(0), "c"(0xC0000101u));

	/* EFER.SCE - bit 0 - is what makes the SYSCALL/SYSRET instructions exist
	 * at all rather than raising #UD. boot.s already set LME and NXE in this
	 * same register; this ORs in one more bit rather than overwriting them. */
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080u));
	low |= 1u;
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080u));

	/* STAR (0xC0000081). SYSCALL loads CS from bits 47:32 and SS from that
	 * value+8. SYSRET, returning to 64-bit mode, loads CS from bits 63:48
	 * plus 16 and SS from that same value plus 8 - with RPL forced to 3
	 * regardless of what is stored here. Working backwards from
	 * GDT_USER_CODE (0x23) and GDT_USER_DATA (0x1B): the sysret field must be
	 * 0x10, which is not a coincidence - it is exactly why gdt.c places user
	 * data one slot before user code. */
	uint64_t star = ((uint64_t) 0x10 << 48) | ((uint64_t) 0x08 << 32);
	low  = (uint32_t)(star & 0xFFFFFFFFu);
	high = (uint32_t)(star >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000081u));

	/* LSTAR (0xC0000082) - where SYSCALL jumps. This is the ONLY thing that
	 * makes SYSCALL go anywhere sensible; everything above is setup for what
	 * happens once it arrives. */
	uint64_t entry = (uint64_t) syscall_entry;
	low  = (uint32_t)(entry & 0xFFFFFFFFu);
	high = (uint32_t)(entry >> 32);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000082u));

	/* FMASK (0xC0000084) - bits to CLEAR in RFLAGS on entry. We clear IF, so
	 * an interrupt cannot land in the handful of instructions before the
	 * stack has been swapped to somewhere safe. A fuller kernel masks TF and
	 * DF here too; IF is the one that would actually break something for us
	 * right now. */
	__asm__ volatile ("wrmsr" : : "a"(0x200u), "d"(0u), "c"(0xC0000084u));
}
