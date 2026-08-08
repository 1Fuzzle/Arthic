/* console.c - kprintf, captured instead of displayed.
 *
 * The real one is in drivers/terminal.c and cannot be used here: it writes to
 * the VGA buffer at physical address 0xB8000 and brackets every call with
 * `cli`/`sti`, an instruction a user-mode process is not permitted to execute.
 * So the tests supply their own kprintf with the same signature and the same
 * format specifiers, writing into a buffer a test can read back.
 *
 * Only the specifiers the kernel actually uses are implemented, and they are
 * implemented by handing the work to the host's vsnprintf - this file is test
 * scaffolding, not kernel code, so it is allowed a libc.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "support.h"
#include "terminal.h"

#define CAPTURE_SIZE 8192

static char   capture[CAPTURE_SIZE];
static size_t capture_length;

void console_reset(void)
{
	capture[0] = '\0';
	capture_length = 0;
}

const char *console_text(void)
{
	return capture;
}

int console_contains(const char *needle)
{
	return strstr(capture, needle) != NULL;
}

void kprintf(const char *fmt, ...)
{
	char    line[1024];
	va_list args;

	va_start(args, fmt);
	/* The kernel's %x means "32-bit unsigned in hex" and %d "int32_t", both of
	 * which the host's vsnprintf reads the same way in a 32-bit build. That is
	 * another reason the tests are compiled -m32. */
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);

	size_t length = strlen(line);
	if (capture_length + length >= CAPTURE_SIZE)
		return;                          /* full; drop rather than overrun */

	memcpy(capture + capture_length, line, length + 1);
	capture_length += length;
}

/* The rest of terminal.h, stubbed. Nothing under test calls these, but a
 * module that did would otherwise fail to link, and a stub is a clearer
 * failure than a missing symbol. */
void terminal_write(const char *str)        { kprintf("%s", str); }
void terminal_putchar(char ch)              { kprintf("%c", ch); }
void terminal_initialise(void)              { }
void terminal_clear(void)                   { console_reset(); }
void terminal_backspace(void)               { }
void terminal_set_colour(uint8_t colour)    { (void) colour; }

uint8_t vga_entry_colour(enum vga_colour fg, enum vga_colour bg)
{
	return (uint8_t)(fg | (bg << 4));
}
