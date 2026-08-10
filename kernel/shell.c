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
#include "string.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "usermode.h"
#include "task.h"
#include "lock.h"
#include "fs.h"
#include "ata.h"
#include "loader.h"
#include "pipe.h"
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
	kprintf("  about         what Arthic is\n");
	kprintf("  ticks         milliseconds-ish since boot, from the timer\n");
	kprintf("  ssptest       deliberately overrun a buffer - should halt cleanly\n");
	kprintf("  echo <text>   print text back\n");
	kprintf("  mem           physical memory usage\n");
	kprintf("  alloc         allocate one 4 KB frame and print its address\n");
	kprintf("  heap          heap usage\n");
	kprintf("  heaptest      exercise kmalloc and kfree\n");
	kprintf("  tasks         list threads\n");
	kprintf("  spin          start a task that loops forever\n");
	kprintf("  kill <id>     terminate a task\n");
	kprintf("  pipetest      a producer and consumer sharing a pipe\n");
	kprintf("  pipestat      pipe contents and how often each side blocked\n");
	kprintf("  install       write the demo program to the disk\n");
	kprintf("  run <name> [arg]  load a program as its own process\n");
	kprintf("                    try: run prog read | write | nx\n");
	kprintf("  ls            list files\n");
	kprintf("  cat <name>    print a file\n");
	kprintf("  write <name> <text>   create a file\n");
	kprintf("  append <name> <text>  add to the end of a file\n");
	kprintf("  append <name> <text>  add to the end of a file\n");
	kprintf("  rm <name>     delete a file\n");
	kprintf("  bigfile       write and verify a 20 KB file (tests indirect blocks)\n");
	kprintf("  df            filesystem usage\n");
	kprintf("  format        create a fresh filesystem (erases the disk)\n");
	kprintf("  racetest      two threads, no lock - watch updates vanish\n");
	kprintf("  locktest      the same test with a mutex\n");
	kprintf("  spawn         start a thread that counts to five\n");
	kprintf("  user          drop to ring 3 and run a user program\n");
	kprintf("  wptest        try to write to kernel code (should be blocked)\n");
	kprintf("  clear         clear the screen\n");
}

static void command_about(void)
{
	kprintf("Arthic v1.8 - a 32-bit x86 kernel written from scratch.\n");
	kprintf("Own GDT and IDT, PIC remapped, timer and keyboard drivers.\n");
	kprintf("Processes, pipes, and files stored as block lists.\n");
}

/* Report physical memory. Frames are 4 KB, so frames * 4 is kilobytes. */
static void command_mem(void)
{
	uint32_t total = pmm_total_frames();
	uint32_t used  = pmm_used_frames();
	uint32_t free  = pmm_free_frames();

	kprintf("  total  %u frames  (%u KB)\n", total, total * 4);
	kprintf("  used   %u frames  (%u KB)\n", used,  used  * 4);
	kprintf("  free   %u frames  (%u KB)\n", free,  free  * 4);
}

/* Take a frame from the allocator and report where it landed. Run it twice and
 * you should get two different addresses — proof the bitmap is being updated
 * rather than handing out the same page forever. */
static void command_alloc(void)
{
	uint32_t addr = pmm_alloc_frame();
	if (addr)
		kprintf("allocated frame at physical 0x%x\n", addr);
	else
		kprintf("out of memory\n");
}

/* Deliberately write to the kernel's own code, which paging_init marked
 * read-only. If protection works this page-faults and halts; if it silently
 * succeeds, something is wrong — most likely CR0.WP is not set.
 *
 * A test that halts the machine is a blunt instrument, but "did the hardware
 * actually stop me" is not a question you can answer any other way. */
/* Deliberately write to the kernel's own code, which paging marked read-only.
 *
 * paging_probe_write arms the fault handler with a resume address first, so the
 * fault is caught and reported rather than fatal. That mechanism is not just
 * for demos: it is exactly what the kernel needs when dereferencing a pointer
 * that came from ring 3 and might be garbage.
 */
static void command_wptest(void)
{
	extern uint32_t kernel_text_start;
	volatile uint32_t *code = (volatile uint32_t *) &kernel_text_start;

	kprintf("writing to kernel code at 0x%x ...\n", (uint32_t) code);

	if (paging_probe_write(code, 0xDEADBEEF))
		kprintf("SUCCEEDED - write protection is NOT working\n");
	else
		kprintf("blocked by the MMU, and the kernel recovered.\n");
}

/* Deliberately overrun a local array's bounds by exactly enough to reach
 * past this function's canary.
 *
 * `volatile` on the buffer is required, not decoration: a plain local array
 * written but never read is dead code to the optimiser, which deleted the
 * entire loop the first time this pattern was written on the 64-bit branch -
 * the canary machinery ran against nothing, and the test always "passed" for
 * a reason unrelated to whether the protector actually worked. */
__attribute__((noinline))
static void smash_the_stack(void)
{
	volatile char buffer[16];

	kprintf("writing 48 bytes into a 16-byte buffer ...\n");

	for (int i = 0; i < 48; i++)
		buffer[i] = 'X';

	kprintf("SURVIVED - the stack protector did not catch this\n");
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

static void command_heap(void)
{
	uint32_t total, used, blocks;
	kheap_stats(&total, &used, &blocks);

	kprintf("  total  %u KB\n", total / 1024);
	kprintf("  used   %u bytes across %u blocks\n", used, blocks);
	kprintf("  free   %u KB\n", (total - used) / 1024);
}

/* Exercise the allocator. Watch the addresses: the third allocation reuses the
 * space freed by the first, which is the whole point of having a free list. */
static void command_heaptest(void)
{
	char *a = (char *) kmalloc(64);
	char *b = (char *) kmalloc(128);

	if (!a || !b) {
		kprintf("allocation failed\n");
		return;
	}

	kprintf("a = kmalloc(64)   -> 0x%x\n", (uint32_t) a);
	kprintf("b = kmalloc(128)  -> 0x%x\n", (uint32_t) b);

	for (int i = 0; i < 63; i++)
		a[i] = 'x';
	a[63] = '\0';
	kprintf("wrote to a, reads back: %c%c%c...\n", a[0], a[1], a[2]);

	kfree(a);
	kprintf("kfree(a)\n");

	char *c = (char *) kmalloc(32);
	kprintf("c = kmalloc(32)   -> 0x%x  %s\n", (uint32_t) c,
	        ((uint32_t) c == (uint32_t) a) ? "(reused a's space)" : "");

	kprintf("double free check: ");
	kfree(c);
	kfree(c);

	kfree(b);
	kprintf("all freed\n");
}

/* A demo thread. Counts to five, a second apart, then returns - and returning
 * lands it in task_exit because that address was planted below the entry point
 * on its stack. */
static void demo_thread(void)
{
	struct task *me = task_current();
	uint32_t id = me ? me->id : 0;

	for (int i = 1; i <= 5; i++) {
		/* Sleep rather than spin. The thread is skipped entirely by the
		 * scheduler until it is due, so waiting now costs nothing at all -
		 * unlike yielding in a loop, which stays runnable and burns a slice
		 * every time round. */
		task_sleep(18);

		kprintf("[task %u] %u of 5\n", id, (uint32_t) i);
	}
}

/* ---- Race conditions -------------------------------------------------------
 *
 * Two threads each add 1 to the same counter, INCREMENTS times. If nothing goes
 * wrong the total is 2 * INCREMENTS. It will not be.
 *
 * The yield in the middle of the update is cheating, and deliberately so. A
 * real race depends on the timer landing in exactly the wrong microsecond, so
 * it might show up once in ten thousand runs - impossible to demonstrate and
 * miserable to debug. Forcing a switch between the read and the write makes the
 * bug reliable so you can see what it actually is. The bug is real either way;
 * only its frequency is rigged.
 */
#define INCREMENTS 200

static volatile uint32_t shared_counter = 0;
static volatile uint32_t workers_done   = 0;
static struct mutex      counter_lock;
static int               use_lock = 0;

static void counter_thread(void)
{
	for (uint32_t i = 0; i < INCREMENTS; i++) {
		if (use_lock)
			mutex_lock(&counter_lock);

		/* counter++ written out longhand, which is all it ever was:
		 * read, modify, write - three steps, interruptible between any two. */
		uint32_t value = shared_counter;
		task_yield();                       /* force the worst case */
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

	kprintf("two threads, %u increments each, %s\n",
	        (uint32_t) INCREMENTS, locked ? "WITH a mutex" : "with NO lock");

	if (!task_create("count", counter_thread) ||
	    !task_create("count", counter_thread)) {
		kprintf("could not create threads\n");
		return;
	}

	while (workers_done < 2)
		task_yield();

	uint32_t expected = 2 * INCREMENTS;

	kprintf("expected %u, got %u", expected, shared_counter);

	if (shared_counter == expected)
		kprintf("  - correct\n");
	else
		kprintf("  - LOST %u updates\n", expected - shared_counter);

	if (locked)
		kprintf("blocked on the lock %u times\n", counter_lock.contended);
}

static void command_racetest(void)
{
	run_counter_test(0);
}

static void command_locktest(void)
{
	run_counter_test(1);
}

/* ---- Filesystem commands ---------------------------------------------------
 *
 * These need arguments, which means splitting a line into a command and the
 * rest. Real shells tokenise properly, handle quoting and expand globs; ours
 * finds the first space. That is enough for `write name text` and honest about
 * what it is.
 */

/* Returns a pointer to the first character after the command word, or 0 if
 * there is nothing there. */
static const char *argument_after(const char *line, const char *command)
{
	if (!kstartswith(line, command))
		return 0;

	const char *p = line;
	while (*p && *p != ' ')
		p++;
	while (*p == ' ')
		p++;

	return *p ? p : 0;
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

	kprintf("  %u blocks total, %u used, %u free  (512 bytes each)\n",
	        total, used, total - used);
	kprintf("  %u files\n", files);
}

static void command_write(const char *line)
{
	const char *rest = argument_after(line, "write ");

	if (!rest) {
		kprintf("usage: write <name> <text>\n");
		return;
	}

	/* Split again: name, then everything after the next space is content. */
	char name[FS_NAME_MAX];
	uint32_t i = 0;

	while (rest[i] && rest[i] != ' ' && i < FS_NAME_MAX - 1) {
		name[i] = rest[i];
		i++;
	}
	name[i] = '\0';

	const char *text = rest + i;
	while (*text == ' ')
		text++;

	uint32_t length = 0;
	while (text[length])
		length++;

	if (length == 0) {
		kprintf("usage: write <name> <text>\n");
		return;
	}

	if (fs_create(name, text, length))
		kprintf("wrote %u bytes to %s\n", length, name);
	else
		kprintf("could not write %s (exists, full, or no space)\n", name);
}

static void command_cat(const char *line)
{
	const char *name = argument_after(line, "cat ");

	if (!name) {
		kprintf("usage: cat <name>\n");
		return;
	}

	static char contents[1024];
	uint32_t size = 0;

	if (!fs_read(name, contents, sizeof(contents) - 1, &size)) {
		kprintf("no such file: %s\n", name);
		return;
	}

	contents[size] = '\0';
	kprintf("%s\n", contents);
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
	while (*text == ' ')
		text++;

	uint32_t length = 0;
	while (text[length])
		length++;

	if (length == 0) {
		kprintf("usage: append <name> <text>\n");
		return;
	}

	if (fs_append(name, text, length))
		kprintf("appended %u bytes to %s\n", length, name);
	else
		kprintf("could not append to %s\n", name);
}

/* Create a file large enough to need the indirect block, then read it back and
 * check every byte.
 *
 * Worth having as a command rather than a one-off test: the indirect path only
 * runs for files over 6 KB, so nothing you do by hand at the shell will ever
 * exercise it. Code that is never run is code that does not work.
 */
static void command_bigfile(void)
{
	if (!fs_is_mounted()) {
		kprintf("no filesystem - run 'format' first\n");
		return;
	}

	const uint32_t size = 20000;      /* well past 12 direct blocks */

	uint8_t *data = (uint8_t *) kmalloc(size);
	if (!data) {
		kprintf("out of heap\n");
		return;
	}

	/* A pattern that catches a block being written to the wrong place: every
	 * byte depends on its own offset, so a swapped or missing block shows up
	 * immediately rather than looking plausible. */
	for (uint32_t i = 0; i < size; i++)
		data[i] = (uint8_t)(i * 7 + (i >> 8));

	fs_delete("bigfile");

	if (!fs_create("bigfile", data, size)) {
		kprintf("could not create it\n");
		kfree(data);
		return;
	}

	uint8_t *back = (uint8_t *) kmalloc(size);
	if (!back) {
		kprintf("out of heap for the read-back\n");
		kfree(data);
		return;
	}

	uint32_t got = 0;
	if (!fs_read("bigfile", back, size, &got)) {
		kprintf("could not read it back\n");
		kfree(data);
		kfree(back);
		return;
	}

	uint32_t bad = 0;
	for (uint32_t i = 0; i < got; i++)
		if (back[i] != data[i])
			bad++;

	kprintf("wrote %u bytes, read back %u, %u wrong\n", size, got, bad);
	kprintf("%s\n", (got == size && bad == 0)
	        ? "indirect blocks work" : "SOMETHING IS WRONG");

	kfree(data);
	kfree(back);
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

static void command_install(void)
{
	if (!fs_is_mounted()) {
		kprintf("no filesystem - run 'format' first\n");
		return;
	}

	if (loader_install("prog"))
		kprintf("wrote the demo program to the disk as 'prog'\n");
	else
		kprintf("could not write it (already there? try 'rm prog')\n");
}

static void command_run(const char *line)
{
	const char *rest = argument_after(line, "run ");

	if (!rest) {
		kprintf("usage: run <name> [argument]\n");
		return;
	}

	/* Split the name from whatever follows it. */
	char name[FS_NAME_MAX];
	uint32_t i = 0;

	while (rest[i] && rest[i] != ' ' && i < FS_NAME_MAX - 1) {
		name[i] = rest[i];
		i++;
	}
	name[i] = '\0';

	const char *args = rest + i;
	while (*args == ' ')
		args++;

	loader_run(name, args);
}

/* ---- Pipes -----------------------------------------------------------------
 *
 * A producer that writes faster than the consumer reads. The pipe holds 256
 * bytes; the producer sends far more than that, so it is forced to wait -
 * which is the whole point. Watch the blocked-write count afterwards.
 */
static struct pipe demo_pipe;
static volatile int pipe_running = 0;

static void producer_thread(void)
{
	char message[] = "message 00 from the producer\n";

	for (int i = 1; i <= 40; i++) {
		message[8]  = (char)('0' + (i / 10) % 10);
		message[9]  = (char)('0' + i % 10);

		uint32_t length = 0;
		while (message[length])
			length++;

		pipe_write(&demo_pipe, message, length);
	}

	pipe_write(&demo_pipe, "END", 3);
	pipe_running--;
}

static void consumer_thread(void)
{
	char chunk[64];

	for (;;) {
		uint32_t got = pipe_read(&demo_pipe, chunk, sizeof(chunk) - 1);
		chunk[got] = '\0';

		/* Deliberately slower than the producer, so the pipe fills up and the
		 * producer has to wait. */
		task_sleep(2);

		int done = 0;
		for (uint32_t i = 0; i + 2 < got; i++) {
			if (chunk[i] == 'E' && chunk[i+1] == 'N' && chunk[i+2] == 'D')
				done = 1;
		}

		kprintf("%s", chunk);

		if (done)
			break;
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

	kprintf("producer sends 40 messages through a %u byte pipe\n",
	        (uint32_t) PIPE_CAPACITY);

	if (!task_create("producer", producer_thread) ||
	    !task_create("consumer", consumer_thread)) {
		kprintf("could not create the threads\n");
		pipe_running = 0;
		return;
	}
}

static void command_pipestat(void)
{
	uint32_t reads, writes;
	pipe_stats(&demo_pipe, &reads, &writes);

	kprintf("  %u bytes waiting in the pipe\n", pipe_available(&demo_pipe));
	kprintf("  reader blocked %u times, writer blocked %u times\n",
	        reads, writes);
}

/* Parse a small unsigned number. No strtoul out here either. */
static uint32_t parse_number(const char *s)
{
	uint32_t value = 0;

	while (*s >= '0' && *s <= '9') {
		value = value * 10 + (uint32_t)(*s - '0');
		s++;
	}
	return value;
}

static void command_kill(const char *line)
{
	const char *arg = argument_after(line, "kill ");

	if (!arg) {
		kprintf("usage: kill <id>   (see 'tasks')\n");
		return;
	}

	uint32_t id = parse_number(arg);
	int result = task_kill(id);

	if (result == 1)
		kprintf("killed task %u\n", id);
	else if (result == -1)
		kprintf("task %u is blocked on something and cannot be killed yet\n", id);
	else
		kprintf("no such task, or it is task 0\n");
}

/* A task that never stops on its own - something for kill to actually be
 * needed for. Without it, kill only ever finishes off threads that were about
 * to exit anyway, which proves nothing. */
static volatile uint32_t spin_counter = 0;

static void spin_thread(void)
{
	for (;;)
		spin_counter++;
}

static void command_spin(void)
{
	uint32_t id = task_create("spin", spin_thread);

	if (id)
		kprintf("task %u is now looping forever - 'kill %u' to stop it\n",
		        id, id);
	else
		kprintf("could not create the task\n");
}

static void command_spawn(void)
{
	uint32_t id = task_create("demo", demo_thread);

	if (id)
		kprintf("spawned task %u - it runs alongside this shell\n", id);
	else
		kprintf("could not create task\n");
}

/* Drop to ring 3, run a small program, come back. */
static void command_user(void)
{
	kprintf("entering ring 3 ...\n");
	usermode_run();
	kprintf("back in ring 0.\n");
}

/* Run whatever is in the buffer. `buffer` is already NUL-terminated by the
 * caller, so it is a valid C string by the time we get here. */
static void execute(const char *line)
{
	if (line[0] == '\0')
		return;                       /* bare Enter: do nothing */

	if (kstrcmp(line, "help") == 0)
		command_help();
	else if (kstrcmp(line, "about") == 0)
		command_about();
	else if (kstrcmp(line, "ticks") == 0)
		command_ticks();
	else if (kstrcmp(line, "ssptest") == 0)
		command_ssptest();
	else if (kstrcmp(line, "mem") == 0)
		command_mem();
	else if (kstrcmp(line, "alloc") == 0)
		command_alloc();
	else if (kstrcmp(line, "heap") == 0)
		command_heap();
	else if (kstrcmp(line, "heaptest") == 0)
		command_heaptest();
	else if (kstrcmp(line, "tasks") == 0)
		task_list();
	else if (kstrcmp(line, "install") == 0)
		command_install();
	else if (kstartswith(line, "run "))
		command_run(line);
	else if (kstrcmp(line, "ls") == 0)
		fs_list();
	else if (kstrcmp(line, "bigfile") == 0)
		command_bigfile();
	else if (kstrcmp(line, "df") == 0)
		command_df();
	else if (kstrcmp(line, "format") == 0)
		command_format();
	else if (kstartswith(line, "write "))
		command_write(line);
	else if (kstartswith(line, "cat "))
		command_cat(line);
	else if (kstartswith(line, "append "))
		command_append(line);
	else if (kstartswith(line, "append "))
		command_append(line);
	else if (kstartswith(line, "rm "))
		command_rm(line);
	else if (kstrcmp(line, "spin") == 0)
		command_spin();
	else if (kstartswith(line, "kill "))
		command_kill(line);
	else if (kstrcmp(line, "pipetest") == 0)
		command_pipetest();
	else if (kstrcmp(line, "pipestat") == 0)
		command_pipestat();
	else if (kstrcmp(line, "racetest") == 0)
		command_racetest();
	else if (kstrcmp(line, "locktest") == 0)
		command_locktest();
	else if (kstrcmp(line, "spawn") == 0)
		command_spawn();
	else if (kstrcmp(line, "user") == 0)
		command_user();
	else if (kstrcmp(line, "wptest") == 0)
		command_wptest();
	else if (kstrcmp(line, "clear") == 0)
		terminal_clear();
	else if (kstrcmp(line, "echo") == 0)
		kprintf("\n");
	else if (kstartswith(line, "echo "))
		kprintf("%s\n", line + 5);    /* skip past "echo " */
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
