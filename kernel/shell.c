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
	kprintf("  echo <text>   print text back\n");
	kprintf("  mem           physical memory usage\n");
	kprintf("  alloc         allocate one 4 KB frame and print its address\n");
	kprintf("  clear         clear the screen\n");
}

static void command_about(void)
{
	kprintf("Arthic v0.9 — a 32-bit x86 kernel written from scratch.\n");
	kprintf("Own GDT and IDT, PIC remapped, timer and keyboard drivers.\n");
	kprintf("Physical memory manager. No paging or filesystem yet.\n");
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

static void command_ticks(void)
{
	uint32_t t = timer_get_ticks();
	kprintf("%u ticks, roughly %u seconds since boot\n", t, t / 18);
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
	else if (kstrcmp(line, "mem") == 0)
		command_mem();
	else if (kstrcmp(line, "alloc") == 0)
		command_alloc();
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
