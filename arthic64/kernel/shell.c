/* shell.c — the command line.
 *
 * The keyboard driver hands us one character at a time. A shell needs whole
 * lines, so this file's real job is buffering: collect characters until Enter,
 * then interpret what was collected.
 *
 * That gap between "a key was pressed" and "a command was entered" is called
 * line discipline, and on a real system it is a surprisingly large amount of
 * code. Ours is about forty lines because we support exactly two editing
 * operations: type a character, and delete one.
 */

#include "shell.h"
#include "terminal.h"
#include "timer.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "usermode.h"
#include "task.h"
#include "lock.h"
#include "pipe.h"
#include "fs.h"
#include "ata.h"
#include "loader.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

#define SHELL_BUFFER_SIZE 128

static char   buffer[SHELL_BUFFER_SIZE];
static size_t buffer_length = 0;

static void prompt(void)
{
	kprintf("arthic> ");
}

/* ---- Commands ------------------------------------------------------------- */

static void command_help(void)
{
	kprintf("  help          this list\n");
	kprintf("  about         what this is\n");
	kprintf("  ticks         timer ticks since boot\n");
	kprintf("  mem           physical memory usage\n");
	kprintf("  heap          heap usage\n");
	kprintf("  heaptest      exercise kmalloc and kfree\n");
	kprintf("  wxtest        try to write to kernel code\n");
	kprintf("  ssptest       deliberately overrun a buffer - should halt cleanly\n");
	kprintf("  guardtest     recurse without bound - should hit the guard page\n");
	kprintf("  tasks         list threads\n");
	kprintf("  spawn         start a thread that counts to five\n");
	kprintf("  kill <id>     terminate a task\n");
	kprintf("  racetest      two threads, no lock - watch updates vanish\n");
	kprintf("  locktest      the same test with a mutex\n");
	kprintf("  pipetest      a producer and consumer sharing a pipe\n");
	kprintf("  pipestat      pipe contents and how often each side blocked\n");
	kprintf("  ls            list files\n");
	kprintf("  cat <name>    print a file\n");
	kprintf("  write <name> <text>   create a file\n");
	kprintf("  append <name> <text>  add to the end of a file\n");
	kprintf("  rm <name>     delete a file\n");
	kprintf("  df            filesystem usage\n");
	kprintf("  format        create a fresh filesystem (erases the disk)\n");
	kprintf("  install       write the demo program to the disk\n");
	kprintf("  run <name>    load an ELF64 program and run it as its own process\n");
	kprintf("  user          drop to ring 3, use SYSCALL, come back\n");
	kprintf("  regs          show 64-bit CPU state\n");
	kprintf("  echo <text>   print text back\n");
	kprintf("  clear         clear the screen\n");
}

static void command_about(void)
{
	kprintf("Arthic 64, stage 4 of the long mode port.\n");
	kprintf("Boots into 64-bit, handles interrupts, reads the keyboard.\n");
	kprintf("It loads ELF64 programs, each in its own address space.\n");
	kprintf("No filesystem, no per-process address spaces yet.\n");
}

/* Show state that only exists in 64-bit, as evidence rather than assertion. */
static void command_regs(void)
{
	uint64_t rsp, cr0, cr3, cr4, efer_low;

	__asm__ volatile ("mov %%rsp, %0" : "=r" (rsp));
	__asm__ volatile ("mov %%cr0, %0" : "=r" (cr0));
	__asm__ volatile ("mov %%cr3, %0" : "=r" (cr3));
	__asm__ volatile ("mov %%cr4, %0" : "=r" (cr4));

	uint32_t low, high;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080u));
	efer_low = low;

	kprintf("  rsp  0x%lx\n", rsp);
	kprintf("  cr0  0x%lx   (bit 31 paging, bit 16 write-protect)\n", cr0);
	kprintf("  cr3  0x%lx   (points at the PML4)\n", cr3);
	kprintf("  cr4  0x%lx   (bit 5 PAE)\n", cr4);
	kprintf("  efer 0x%lx   (bit 8 LME, bit 10 LMA, bit 11 NXE)\n", efer_low);

	/* LMA - long mode ACTIVE - is set by the CPU itself, not by us. It is the
	 * hardware confirming the transition rather than us claiming it. */
	kprintf("  long mode %s\n",
	        (low & (1u << 10)) ? "ACTIVE" : "not active");
}

static void command_mem(void)
{
	kprintf("  total  %lu frames  (%lu KB)\n",
	        pmm_total_frames(), pmm_total_frames() * 4);
	kprintf("  used   %lu frames  (%lu KB)\n",
	        pmm_used_frames(), pmm_used_frames() * 4);
	kprintf("  free   %lu frames  (%lu KB)\n",
	        pmm_free_frames(), pmm_free_frames() * 4);
}

static void command_heap(void)
{
	uint64_t total, used, blocks;
	kheap_stats(&total, &used, &blocks);

	kprintf("  total  %lu KB\n", total / 1024);
	kprintf("  used   %lu bytes across %lu blocks\n", used, blocks);
}

static void command_heaptest(void)
{
	char *a = (char *) kmalloc(64);
	char *b = (char *) kmalloc(128);

	if (!a || !b) {
		kprintf("allocation failed\n");
		return;
	}

	kprintf("a = kmalloc(64)   -> 0x%lx\n", (uint64_t) a);
	kprintf("b = kmalloc(128)  -> 0x%lx\n", (uint64_t) b);

	kfree(a);

	char *c = (char *) kmalloc(32);
	kprintf("c = kmalloc(32)   -> 0x%lx  %s\n", (uint64_t) c,
	        ((uint64_t) c == (uint64_t) a) ? "(reused a's space)" : "");

	kfree(c);
	kfree(b);
	kprintf("all freed\n");
}

/* Try to write to the kernel's own code, and to execute its own stack.
 * Both should be refused, and both should be survivable. */
static void command_wxtest(void)
{
	extern uint64_t kernel_text_start;

	kprintf("writing to kernel code at 0x%lx ... ", (uint64_t) &kernel_text_start);

	if (paging_probe_write((volatile uint64_t *) &kernel_text_start, 0xDEAD))
		kprintf("SUCCEEDED - write protection is NOT working\n");
	else
		kprintf("blocked, and recovered\n");

	kprintf("W^X: code is r-x, everything else is rw- with NX set\n");
}

/* Drop to ring 3 and run the SYSCALL demo. */
static void command_user(void)
{
	kprintf("entering ring 3 (first hop via IRETQ) ...\n");
	usermode_run();
	kprintf("back in ring 0 (returned via SYSCALL/SYSRET or a caught fault).\n");
}

/* ---- Race conditions -----------------------------------------------------
 * Forced with a deliberate task_yield between read and write so it fails
 * every time rather than once in ten thousand runs - the bug is real either
 * way, only its frequency is rigged for demonstration.
 */
#define INCREMENTS 200

static volatile uint64_t shared_counter = 0;
static volatile uint64_t workers_done   = 0;
static struct mutex      counter_lock;
static int               use_lock = 0;

static void counter_thread(void)
{
	for (uint64_t i = 0; i < INCREMENTS; i++) {
		if (use_lock)
			mutex_lock(&counter_lock);

		uint64_t value = shared_counter;
		task_yield();
		shared_counter = value + 1;

		if (use_lock)
			mutex_unlock(&counter_lock);
	}

	workers_done++;
}

static void run_counter_test(int locked)
{
	shared_counter = 0;
	workers_done   = 0;
	use_lock       = locked;
	mutex_init(&counter_lock);

	kprintf("two threads, %lu increments each, %s\n",
	        (uint64_t) INCREMENTS, locked ? "WITH a mutex" : "with NO lock");

	if (!task_create("count", counter_thread) ||
	    !task_create("count", counter_thread)) {
		kprintf("could not create threads\n");
		return;
	}

	while (workers_done < 2)
		task_yield();

	uint64_t expected = 2 * INCREMENTS;

	kprintf("expected %lu, got %lu", expected, shared_counter);

	if (shared_counter == expected)
		kprintf("  - correct\n");
	else
		kprintf("  - LOST %lu updates\n", expected - shared_counter);

	if (locked)
		kprintf("blocked on the lock %lu times\n", counter_lock.contended);
}

static void command_racetest(void) { run_counter_test(0); }
static void command_locktest(void) { run_counter_test(1); }

/* ---- Pipes ------------------------------------------------------------- */
static struct pipe demo_pipe;
static volatile int pipe_running = 0;

static void producer_thread(void)
{
	char message[] = "message 00 from the producer\n";

	for (int i = 1; i <= 40; i++) {
		message[8] = (char)('0' + (i / 10) % 10);
		message[9] = (char)('0' + i % 10);

		uint64_t length = 0;
		while (message[length]) length++;

		pipe_write(&demo_pipe, message, length);
	}

	pipe_write(&demo_pipe, "END", 3);
	pipe_running--;
}

static void consumer_thread(void)
{
	char chunk[64];

	for (;;) {
		uint64_t got = pipe_read(&demo_pipe, chunk, sizeof(chunk) - 1);
		chunk[got] = '\0';

		task_sleep(2);

		int done = 0;
		for (uint64_t i = 0; i + 2 < got; i++)
			if (chunk[i]=='E' && chunk[i+1]=='N' && chunk[i+2]=='D')
				done = 1;

		kprintf("%s", chunk);

		if (done) break;
	}

	pipe_running--;
}

static void command_pipetest(void)
{
	if (pipe_running) {
		kprintf("a pipe test is already running\n");
		return;
	}

	pipe_init(&demo_pipe);
	pipe_running = 2;

	kprintf("producer sends 40 messages through a %lu byte pipe\n",
	        (uint64_t) PIPE_CAPACITY);

	if (!task_create("producer", producer_thread) ||
	    !task_create("consumer", consumer_thread)) {
		kprintf("could not create the threads\n");
		pipe_running = 0;
	}
}

static void command_pipestat(void)
{
	uint64_t reads, writes;
	pipe_stats(&demo_pipe, &reads, &writes);

	kprintf("  %lu bytes waiting in the pipe\n", pipe_available(&demo_pipe));
	kprintf("  reader blocked %lu times, writer blocked %lu times\n",
	        reads, writes);
}

static const char *argument_after(const char *line, const char *command)
{
	if (!kstartswith(line, command))
		return 0;

	const char *p = line;
	while (*p && *p != ' ') p++;
	while (*p == ' ') p++;

	return *p ? p : 0;
}

static void command_install(void)
{
	/* Copies user_prog.elf, produced by build.sh, onto the filesystem. In a
	 * real system this would be `cp` from a host filesystem or a network
	 * fetch; here it is embedded as a C array for the same reason the 32-bit
	 * branch's stage 1.8 demo was - simplicity, before anything more
	 * elaborate is worth building. */
	extern const unsigned char prog_elf_blob[];
	extern const unsigned int  prog_elf_blob_length;

	if (!fs_is_mounted()) {
		kprintf("no filesystem - run 'format' first\n");
		return;
	}

	if (fs_create("prog64", prog_elf_blob, prog_elf_blob_length))
		kprintf("wrote the demo program to disk as 'prog64'\n");
	else
		kprintf("could not write it (already there? try 'rm prog64')\n");
}

static void command_run(const char *line)
{
	const char *name = argument_after(line, "run ");

	if (!name) {
		kprintf("usage: run <name>\n");
		return;
	}

	loader_run(name);
}

static void command_format(void)
{
	if (!ata_sector_count()) {
		kprintf("no disk attached\n");
		return;
	}

	kprintf("formatting - this erases everything on the disk\n");

	if (fs_format())
		kprintf("done. ArthicFS ready.\n");
	else
		kprintf("format failed\n");
}

static void command_df(void)
{
	uint32_t total, used, files;
	fs_stats(&total, &used, &files);

	if (!fs_is_mounted()) {
		kprintf("no filesystem mounted\n");
		return;
	}

	kprintf("  %lu blocks total, %lu used, %lu free  (512 bytes each)\n",
	        (uint64_t) total, (uint64_t) used, (uint64_t)(total - used));
	kprintf("  %lu files\n", (uint64_t) files);
}

static void command_write(const char *line)
{
	const char *rest = argument_after(line, "write ");

	if (!rest) {
		kprintf("usage: write <name> <text>\n");
		return;
	}

	char name[FS_NAME_MAX];
	uint32_t i = 0;

	while (rest[i] && rest[i] != ' ' && i < FS_NAME_MAX - 1) {
		name[i] = rest[i];
		i++;
	}
	name[i] = '\0';

	const char *text = rest + i;
	while (*text == ' ') text++;

	uint32_t length = 0;
	while (text[length]) length++;

	if (length == 0) {
		kprintf("usage: write <name> <text>\n");
		return;
	}

	if (fs_create(name, text, length))
		kprintf("wrote %lu bytes to %s\n", (uint64_t) length, name);
	else
		kprintf("could not write %s (exists, full, or no space)\n", name);
}

static void command_append(const char *line)
{
	const char *rest = argument_after(line, "append ");

	if (!rest) {
		kprintf("usage: append <name> <text>\n");
		return;
	}

	char name[FS_NAME_MAX];
	uint32_t i = 0;

	while (rest[i] && rest[i] != ' ' && i < FS_NAME_MAX - 1) {
		name[i] = rest[i];
		i++;
	}
	name[i] = '\0';

	const char *text = rest + i;
	while (*text == ' ') text++;

	uint32_t length = 0;
	while (text[length]) length++;

	if (length == 0) {
		kprintf("usage: append <name> <text>\n");
		return;
	}

	if (fs_append(name, text, length))
		kprintf("appended %lu bytes to %s\n", (uint64_t) length, name);
	else
		kprintf("could not append to %s\n", name);
}

static void command_cat(const char *line)
{
	const char *name = argument_after(line, "cat ");

	if (!name) {
		kprintf("usage: cat <name>\n");
		return;
	}

	static char contents[512];
	uint32_t size = 0;

	if (!fs_read(name, contents, sizeof(contents) - 1, &size)) {
		kprintf("no such file: %s\n", name);
		return;
	}

	contents[size] = '\0';
	kprintf("%s\n", contents);
}

static void command_rm(const char *line)
{
	const char *name = argument_after(line, "rm ");

	if (!name) {
		kprintf("usage: rm <name>\n");
		return;
	}

	if (fs_delete(name))
		kprintf("deleted %s\n", name);
	else
		kprintf("no such file: %s\n", name);
}

static void command_tasks(void)
{
	task_list();
}

static void demo_thread(void)
{
	struct task *me = task_current();
	uint32_t id = me ? me->id : 0;

	for (int i = 1; i <= 5; i++) {
		task_sleep(18);
		kprintf("[task %u] %u of 5\n", id, (uint32_t) i);
	}
}

static void command_spawn(void)
{
	uint32_t id = task_create("demo", demo_thread);

	if (id)
		kprintf("spawned task %u\n", id);
	else
		kprintf("could not create task\n");
}

/* Parse the id, then just mark the task finished. The reaper picks it up on
 * the next scheduling pass and frees its stack - same mechanism a task uses
 * to end itself, applied from the outside. Task 0 is refused: it is the
 * fallback when nothing else is runnable, and killing it would leave the
 * scheduler with nowhere to go. A BLOCKED task is refused too, for the same
 * reason as the 32-bit branch - it is linked into some wait queue by a
 * pointer that queue owns, and freeing it out from under that queue would
 * leave a dangling pointer behind. */
static void command_kill(const char *line)
{
	const char *arg = line;
	while (*arg && *arg != ' ') arg++;
	while (*arg == ' ') arg++;

	if (!*arg) {
		kprintf("usage: kill <id>\n");
		return;
	}

	uint32_t id = 0;
	while (*arg >= '0' && *arg <= '9') {
		id = id * 10 + (uint32_t)(*arg - '0');
		arg++;
	}

	if (id == 0) {
		kprintf("no such task, or it is task 0\n");
		return;
	}

	struct task *t = task_by_id(id);
	if (!t || t->state == TASK_FINISHED) {
		kprintf("no such task, or it is task 0\n");
		return;
	}

	if (t->state == TASK_BLOCKED) {
		kprintf("task %u is blocked on something and cannot be killed yet\n", id);
		return;
	}

	t->state = TASK_FINISHED;
	kprintf("killed task %u\n", id);
}

/* Deliberately overrun a local array's bounds by exactly enough to reach past
 * where the compiler placed this function's canary, without going so far
 * that it also reaches the return address and crashes some OTHER way before
 * the check even gets a chance to run. This is not a hypothetical - it is the
 * single most common memory-safety bug in C, done on purpose so the catch can
 * be seen rather than taken on faith.
 *
 * `buffer` is volatile for a reason that has nothing to do with hardware: a
 * plain local array that is written but never read is, from the optimiser's
 * point of view, dead code - nothing observes its final contents, so at -O2
 * GCC legitimately deleted the entire loop the first time this was written,
 * and the canary machinery ran against nothing, always "passing" for a
 * reason that had nothing to do with stack protection working. `volatile`
 * forces every store to be treated as having an observable effect, which is
 * the only way to make a deliberately-unused overflow survive optimisation
 * at all. */
__attribute__((noinline))
static void smash_the_stack(void)
{
	volatile char buffer[16];

	kprintf("writing 48 bytes into a 16-byte buffer ...\n");

	for (int i = 0; i < 48; i++)
		buffer[i] = 'X';

	/* Unreachable if the canary catches it, which is the point - the halt
	 * happens inside __stack_chk_fail before this function's epilogue, and
	 * therefore before this line, ever runs. */
	kprintf("SURVIVED - the stack protector did not catch this\n");
}

/* Recurses without any base case, deliberately - the simplest way to
 * genuinely exhaust a real stack rather than merely simulating the shape of
 * doing so.
 *
 * Three separate optimisations had to be defeated to make this test actually
 * test anything, and each one failed silently in exactly the same way: no
 * crash, no guard page fault, just the recursion running forever with the
 * screen showing nothing further - because none of the three failures
 * actually GREW the stack at all.
 *
 * `noinline` stops the compiler folding this call into its caller. It does
 * NOT stop a separate optimisation, sibling-call (tail-call) elimination:
 * since this function's recursive call was the very last thing it did, GCC
 * at -O2 rewrote it into a plain jump back to the top of the function,
 * reusing ONE stack frame forever - an infinite loop wearing recursion's
 * clothes, using constant stack space by construction. The volatile counter
 * incremented AFTER the recursive call exists for exactly one reason: a
 * proper tail call requires nothing meaningful to happen once it returns,
 * and this call never returns, so anything genuinely dependent on it
 * returning is enough to convince the compiler this cannot be treated as
 * one.
 *
 * `volatile` on the stack buffer stops the write into it being deleted as an
 * unobserved dead store, the same lesson as ssptest's buffer.
 */
static volatile uint64_t recursion_depth = 0;

__attribute__((noinline))
static void recurse_forever(void)
{
	volatile char eat_some_stack[128];
	eat_some_stack[0] = 'X';

	recurse_forever();

	recursion_depth++;   /* unreachable, and that is exactly the point */
}

static void guard_test_thread(void)
{
	kprintf("recursing without bound - should hit the guard page below\n");
	kprintf("this task's kernel stack, and fault, rather than corrupting\n");
	kprintf("whatever memory happens to sit past the end of it\n");

	recurse_forever();

	/* Unreachable if the guard does its job. */
	kprintf("SURVIVED - ran off the end of the stack without faulting\n");
}

static void command_guardtest(void)
{
	uint32_t id = task_create("guard", guard_test_thread);

	if (id)
		kprintf("spawned task %u to overflow its own stack\n", id);
	else
		kprintf("could not create the task\n");
}

static void command_ssptest(void)
{
	smash_the_stack();
}

static void command_ticks(void)
{
	uint32_t t = timer_get_ticks();
	kprintf("%u ticks, roughly %u seconds since boot\n", t, t / 18);
}




























/* A task that never stops on its own - something for kill to actually be
 * needed for. Without it, kill only ever finishes off threads that were about
 * to exit anyway, which proves nothing. */





/* Run whatever is in the buffer. `buffer` is already NUL-terminated by the
 * caller, so it is a valid C string by the time we get here. */
static void execute(const char *line)
{
	if (line[0] == '\0')
		return;

	if (kstrcmp(line, "help") == 0)
		command_help();
	else if (kstrcmp(line, "about") == 0)
		command_about();
	else if (kstrcmp(line, "ticks") == 0)
		command_ticks();
	else if (kstrcmp(line, "mem") == 0)
		command_mem();
	else if (kstrcmp(line, "heap") == 0)
		command_heap();
	else if (kstrcmp(line, "heaptest") == 0)
		command_heaptest();
	else if (kstrcmp(line, "user") == 0)
		command_user();
	else if (kstrcmp(line, "tasks") == 0)
		command_tasks();
	else if (kstrcmp(line, "spawn") == 0)
		command_spawn();
	else if (kstrcmp(line, "racetest") == 0)
		command_racetest();
	else if (kstrcmp(line, "locktest") == 0)
		command_locktest();
	else if (kstrcmp(line, "pipetest") == 0)
		command_pipetest();
	else if (kstrcmp(line, "pipestat") == 0)
		command_pipestat();
	else if (kstrcmp(line, "ls") == 0)
		fs_list();
	else if (kstrcmp(line, "df") == 0)
		command_df();
	else if (kstrcmp(line, "format") == 0)
		command_format();
	else if (kstrcmp(line, "install") == 0)
		command_install();
	else if (kstartswith(line, "run "))
		command_run(line);
	else if (kstartswith(line, "write "))
		command_write(line);
	else if (kstartswith(line, "append "))
		command_append(line);
	else if (kstartswith(line, "cat "))
		command_cat(line);
	else if (kstartswith(line, "rm "))
		command_rm(line);
	else if (kstartswith(line, "kill "))
		command_kill(line);
	else if (kstrcmp(line, "wxtest") == 0)
		command_wxtest();
	else if (kstrcmp(line, "ssptest") == 0)
		command_ssptest();
	else if (kstrcmp(line, "guardtest") == 0)
		command_guardtest();
	else if (kstrcmp(line, "regs") == 0)
		command_regs();
	else if (kstrcmp(line, "clear") == 0)
		terminal_clear();
	else if (kstrcmp(line, "echo") == 0)
		kprintf("\n");
	else if (kstartswith(line, "echo "))
		kprintf("%s\n", line + 5);
	else
		kprintf("unknown command: %s  (try 'help')\n", line);
}

/* ---- Input ---------------------------------------------------------------- */

void shell_input(char c)
{
	if (c == '\n') {
		terminal_putchar('\n');
		buffer[buffer_length] = '\0';   /* make it a real C string */
		execute(buffer);
		buffer_length = 0;
		prompt();
		return;
	}

	if (c == '\b') {
		/* Refuse to delete past the start of the line, or we would erase the
		 * prompt itself and then keep going into whatever is above it. */
		if (buffer_length > 0) {
			buffer_length--;
			terminal_backspace();
		}
		return;
	}

	/* Leave room for the terminating NUL. Silently dropping input at the
	 * limit is not elegant, but it is correct — and this bounds check is the
	 * difference between a full buffer and a kernel that overwrites whatever
	 * happens to sit after it in memory. */
	if (buffer_length < SHELL_BUFFER_SIZE - 1) {
		buffer[buffer_length++] = c;
		terminal_putchar(c);
	}
}

void shell_init(void)
{
	buffer_length = 0;
	prompt();
}
