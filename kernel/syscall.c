/* syscall.c — handling requests from ring 3.
 *
 * Ring 3 executes `int $0x80`. The CPU looks up vector 0x80 in the IDT, sees a
 * gate whose DPL is 3 (the only one), switches to the kernel stack named in the
 * TSS, and lands in the same stub machinery every other interrupt uses.
 *
 * VALIDATION IS THE WHOLE JOB
 *
 * Everything arriving here was chosen by code the kernel does not trust. A
 * pointer from ring 3 might point at kernel memory, at unmapped memory, or at
 * the syscall table itself. If the kernel dereferences it without checking, the
 * user program has just made the kernel read or write on its behalf, at full
 * privilege. That is the confused deputy problem, and it is how a great many
 * real privilege escalations work.
 *
 * So: check every pointer against the range ring 3 is actually allowed, and
 * bound every length. Twice as much code as the useful part, which is roughly
 * the correct ratio.
 */

#include "syscall.h"
#include "idt.h"
#include "terminal.h"
#include "timer.h"
#include "usermode.h"
#include "paging.h"

/* The window ring 3 may legitimately reference. Set by usermode_init. */
static uint32_t user_range_start = 0;
static uint32_t user_range_end   = 0;

void syscall_set_user_range(uint32_t start, uint32_t end)
{
	user_range_start = start;
	user_range_end   = end;
}

/* Is a NUL-terminated string from ring 3 safe to read?
 *
 * Two separate checks, both necessary. The pointer must start inside the user
 * range, and the string must terminate before the end of it — otherwise a
 * string with no NUL would walk straight out of user memory and into whatever
 * follows, and the kernel would happily print it.
 *
 * The length cap is a third line of defence: even a valid pointer should not be
 * able to make the kernel loop for an unbounded time.
 */
static int user_string_ok(uint32_t ptr, uint32_t max_length)
{
	if (ptr < user_range_start || ptr >= user_range_end)
		return 0;

	for (uint32_t i = 0; i < max_length; i++) {
		uint32_t addr = ptr + i;

		if (addr >= user_range_end)
			return 0;                    /* ran off the end without a NUL */

		if (*(const char *) addr == '\0')
			return 1;                    /* properly terminated in range  */
	}

	return 0;                            /* too long */
}

static void syscall_dispatch(struct registers *regs)
{
	switch (regs->eax) {

	case SYS_WRITE:
		if (!user_string_ok(regs->ebx, 256)) {
			/* Refuse, and say so. Silently ignoring a bad argument teaches
			 * a buggy program nothing and hides an attack. */
			kprintf("[syscall] rejected bad string pointer 0x%x\n", regs->ebx);
			regs->eax = (uint32_t) -1;
			return;
		}
		terminal_write((const char *) regs->ebx);
		regs->eax = 0;
		return;

	case SYS_TICKS:
		/* Returning a value means writing it into the saved eax, which iret
		 * restores. The user program sees it as the result of `int $0x80`. */
		regs->eax = timer_get_ticks();
		return;

	case SYS_EXIT:
		usermode_exit();     /* does not return */
		return;

	default:
		kprintf("[syscall] unknown call %u\n", regs->eax);
		regs->eax = (uint32_t) -1;
		return;
	}
}

void syscall_install(void)
{
	isr_install_handler_vector(SYSCALL_VECTOR, syscall_dispatch);
}
