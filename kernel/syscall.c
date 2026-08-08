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
 * So: check every pointer against what ring 3 is actually allowed to touch,
 * and bound every length. Twice as much code as the useful part, which is
 * roughly the correct ratio.
 *
 * WHO DECIDES WHAT IS ALLOWED
 *
 * The page tables do, and nothing else. Every check below goes through
 * paging_user_access_ok, which walks the address space currently loaded and
 * asks whether ring 3 could reach those bytes on its own.
 *
 * The alternative - remembering a start and end address when a program is set
 * up and comparing against those - reads as simpler and is wrong twice over.
 * It can disagree with the hardware, and one pair of variables cannot describe
 * two tasks with different address spaces, so whichever ran last decides what
 * the other is permitted to do. Deriving the answer from the tables makes both
 * problems impossible rather than merely handled.
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

/* Is a buffer of `length` bytes at `ptr` one ring 3 may hand over?
 *
 * The length cap comes first and is not just tidiness: it bounds how much work
 * one syscall can ask the kernel to do, whatever the pointer turns out to be.
 *
 * `written` says which direction the bytes travel. A buffer the kernel fills
 * in has to be writable by ring 3 as well as readable, because a program that
 * could name a read-only page here would be using the kernel to write to pages
 * the MMU refuses it directly.
 */
static int user_buffer_ok(uint32_t ptr, uint32_t length, int written)
{
	if (length == 0 || length > 4096)
		return 0;

	return paging_user_access_ok(ptr, length, written);
}

/* Is a NUL-terminated string from ring 3 safe to read?
 *
 * Harder than a buffer, because the length is not given - it is wherever the
 * NUL happens to be, and a string with no NUL at all must not be allowed to
 * walk the kernel out of user memory and into whatever follows.
 *
 * So each byte is checked before it is read, rather than checking a span up
 * front: the string may legitimately end one byte before a page that ring 3
 * cannot touch, and demanding the whole `max_length` be accessible would
 * reject it. The cap remains as the outer bound on how long the kernel will
 * look.
 */
static int user_string_ok(uint32_t ptr, uint32_t max_length)
{
	for (uint32_t i = 0; i < max_length; i++) {
		uint32_t addr = ptr + i;

		if (addr < ptr)                      /* wrapped past the top of memory */
			return 0;

		if (!paging_user_access_ok(addr, 1, 0))
			return 0;

		if (*(const char *) addr == '\0')
			return 1;                        /* properly terminated */
	}

	return 0;                                /* too long */
}

/* Buffer a program's output until a line is complete.
 *
 * Without this, `print("line "); print("12\n");` from one process can have
 * another process's output land between the two calls - which is exactly what
 * you see if you look: "line 1  [writer] finished" then "1 through the pipe".
 * Each write was atomic; the sequence was not.
 *
 * Real terminals do the same thing, and it is the other half of line
 * discipline - the shell already does the input side.
 *
 * The buffer is per-task, so two processes building lines at the same time do
 * not corrupt each other's. It flushes on a newline or when full; the second
 * condition matters because a program that never emits one must not be able to
 * make the kernel buffer without limit.
 */
static void buffered_write(struct task *t, const char *text)
{
	if (!t) {
		terminal_write(text);        /* no task context - just print it */
		return;
	}

	for (uint32_t i = 0; text[i]; i++) {
		t->outbuf[t->outlen++] = text[i];

		if (text[i] == '\n' || t->outlen >= sizeof(t->outbuf) - 1) {
			t->outbuf[t->outlen] = '\0';
			terminal_write(t->outbuf);
			t->outlen = 0;
		}
	}
}

void syscall_flush_output(struct task *t)
{
	if (t && t->outlen) {
		t->outbuf[t->outlen] = '\0';
		terminal_write(t->outbuf);
		t->outlen = 0;
	}
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
		buffered_write(task_current(), (const char *) regs->ebx);
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

		/* The kernel only reads this one. */
		if (!user_buffer_ok(regs->ebx, regs->ecx, 0)) {
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

		/* And this one the kernel writes into, so it must be writable from
		 * ring 3 too - see user_buffer_ok. */
		if (!user_buffer_ok(regs->ebx, regs->ecx, 1)) {
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
	isr_install_handler_vector(SYSCALL_VECTOR, syscall_dispatch);
}
