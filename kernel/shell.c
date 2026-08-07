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
#include <stddef.h>
#include <stdint.h>

#define SHELL_BUFFER_SIZE 128

static char   buffer[SHELL_BUFFER_SIZE];
static size_t buffer_length = 0;

/* ---- Our own string functions ---------------------------------------------
 * No string.h. If we want to compare strings we write it.
 *
 * Both of these walk two pointers forward in step. kstrcmp returns 0 for
 * equal, which matches the convention of the real strcmp — worth keeping even
 * though it reads backwards, because every C programmer expects it.
 */
static int kstrcmp(const char *a, const char *b)
{
	while (*a && (*a == *b)) {
		a++;
		b++;
	}
	return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

/* Does `str` begin with `prefix`? Used to spot "echo " followed by anything. */
static int kstartswith(const char *str, const char *prefix)
{
	while (*prefix) {
		if (*str != *prefix)
			return 0;
		str++;
		prefix++;
	}
	return 1;
}

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
	kprintf("  clear         clear the screen\n");
}

static void command_about(void)
{
	kprintf("Arthic v0.7 — a 32-bit x86 kernel written from scratch.\n");
	kprintf("Own GDT and IDT, PIC remapped, timer and keyboard drivers.\n");
	kprintf("No memory manager and no filesystem. Yet.\n");
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
