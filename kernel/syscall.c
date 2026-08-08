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
#include "task.h"
#include "pipe.h"

/* The one channel programs share. A real system would give each pipe an
 * identifier and let a program hold several; one global channel is enough to
 * show two processes talking, and honest about being a demonstration. */
static struct pipe ipc_pipe;
static int ipc_ready = 0;

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
/* Is a buffer of `length` bytes at `ptr` entirely inside the user region?
 *
 * Three checks, and all three are load-bearing. The start must be in range.
 * The length must be sane on its own - an enormous one would make the addition
 * below wrap around and produce an end address that looks fine. And the end
 * must be in range too.
 *
 * That middle check is the one people forget, and integer overflow in a bounds
 * check is how a great many exploits begin: ptr + length wraps past zero, the
 * comparison passes, and the kernel then copies gigabytes.
 */
static int user_buffer_ok(uint32_t ptr, uint32_t length)
{
	if (length == 0 || length > 4096)
		return 0;

	if (ptr < user_range_start || ptr >= user_range_end)
		return 0;

	if (ptr + length < ptr)          /* overflow */
		return 0;

	if (ptr + length > user_range_end)
		return 0;

	return 1;
}

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

/* A program's output is buffered until a line is complete.
 *
 * Without that, `print("line "); print("12\n");` from one process can have
 * another process's output land between the two calls - which is exactly what
 * you see if you look: "line 1  [writer] finished" then "1 through the pipe".
 * Each write was atomic; the sequence was not.
 *
 * Real terminals do the same thing, and it is the other half of line
 * discipline - the shell already does the input side.
 *
 * The buffer itself belongs to the task, and so do the two functions that
 * touch it, in task.c. This file used to keep a second copy of that logic
 * writing a second, identically-purposed field, and the two drifted: the flush
 * on task death read the field nobody wrote, so a program killed mid-line had
 * that line thrown away rather than printed.
 */
void syscall_flush_output(struct task *t)
{
	task_flush_output(t);
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
		task_buffer_output(task_current(), (const char *) regs->ebx);
		regs->eax = 0;
		return;

	case SYS_TICKS:
		/* Returning a value means writing it into the saved eax, which iret
		 * restores. The user program sees it as the result of `int $0x80`. */
		regs->eax = timer_get_ticks();
		return;

	case SYS_SLEEP:
		/* Cap it. An unbounded sleep from ring 3 is a request the kernel
		 * should not honour without limit - a program should not be able to
		 * park a kernel thread forever by passing a huge number. */
		task_sleep(regs->ebx > 200 ? 200 : regs->ebx);
		regs->eax = 0;
		return;

	case SYS_ID: {
		struct task *t = task_current();
		regs->eax = t ? t->id : 0;
		return;
	}

	case SYS_PIPE_WRITE:
		if (!ipc_ready) {
			pipe_init(&ipc_pipe);
			ipc_ready = 1;
		}

		if (!user_buffer_ok(regs->ebx, regs->ecx)) {
			kprintf("[syscall] rejected bad write buffer 0x%x len %u\n",
			        regs->ebx, regs->ecx);
			regs->eax = (uint32_t) -1;
			return;
		}

		regs->eax = pipe_write(&ipc_pipe, (const char *) regs->ebx, regs->ecx);
		return;

	case SYS_PIPE_READ:
		if (!ipc_ready) {
			pipe_init(&ipc_pipe);
			ipc_ready = 1;
		}

		if (!user_buffer_ok(regs->ebx, regs->ecx)) {
			kprintf("[syscall] rejected bad read buffer 0x%x len %u\n",
			        regs->ebx, regs->ecx);
			regs->eax = (uint32_t) -1;
			return;
		}

		regs->eax = pipe_read(&ipc_pipe, (char *) regs->ebx, regs->ecx);
		return;

	case SYS_EXIT: {
		struct task *t = task_current();

		/* Anything half-written should still be seen. */
		syscall_flush_output(t);

		/* A loaded program is a task of its own and dies as one. The built-in
		 * ring 3 demo runs on the shell's task, which must survive, so that
		 * one unwinds instead. */
		if (t && t->on_exit)
			task_terminate();    /* does not return */

		usermode_exit();         /* does not return */
		return;
	}

	default:
		kprintf("[syscall] unknown call %u\n", regs->eax);
		regs->eax = (uint32_t) -1;
		return;
	}
}

void syscall_install(void)
{
	/* Ring 3 has no other way in. If this registration were dropped, `int 0x80`
	 * would reach the default exception handler and every program would die on
	 * its first syscall, with nothing pointing back to here. */
	if (!isr_install_handler_vector(SYSCALL_VECTOR, syscall_dispatch))
		kpanic("syscall: could not install the int 0x80 handler");
}
