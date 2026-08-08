/* terminal.c — VGA text output.
 *
 * The screen is memory. Address 0xB8000 holds an 80x25 grid of two-byte cells:
 * one byte character, one byte colour. Write there and characters appear. That
 * is the entire mechanism.
 *
 * Also here: the hardware cursor, which the VGA controller draws for us once we
 * tell it where to sit over I/O ports; and kprintf, because printing a number
 * means printing characters, and this is where characters live.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "terminal.h"
#include "serial.h"
#include "io.h"
#include "irq.h"

/* ---- serialising the console ----------------------------------------------
 * Two tasks writing at once interleave mid-word: you get "kill 1[task 5] 2 of"
 * instead of two clean lines. Same shape as the counter in racetest, just with
 * characters instead of arithmetic.
 *
 * The fix is NOT a mutex, and the reason matters. kprintf is called from
 * interrupt handlers - the page fault reporter, for one. A mutex blocks, and
 * blocking inside an interrupt handler means the handler never returns, which
 * is a deadlock rather than a delay.
 *
 * A console lock must therefore be something that cannot block. On one CPU,
 * disabling interrupts is exactly that: nothing else can run to interleave
 * with us, whether it is another task or another interrupt. It is a blunt tool
 * and it is the right one, which is why real kernels do the same for their
 * emergency print paths.
 *
 * Which is precisely what irq_save gives the scheduler and the pipes, so this
 * uses that rather than keeping a second copy of the same two instructions.
 * The names stay, because "take the console lock" says why we are doing it.
 */
#define console_lock()        irq_save()
#define console_unlock(flags) irq_restore(flags)

/* ---- Types ----------------------------------------------------------------
 * stdint.h and stddef.h are two of the very few headers that are safe here,
 * because they contain no code at all — only type definitions.
 *
 *   uint8_t   = unsigned integer, exactly 8 bits   (0 to 255)
 *   uint16_t  = unsigned integer, exactly 16 bits  (0 to 65535)
 *   size_t    = the type used for sizes and counts
 *
 * In OS work you use these instead of int/char because you are describing
 * hardware layouts, and "exactly 16 bits" is a promise plain `int` won't make.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>   /* variadic arguments — also freestanding-safe */

#include "gdt.h"      /* our own header — quotes, not angle brackets */
#include "idt.h"
#include "io.h"        /* outb / inb now live here, shared with idt.c */
#include "terminal.h"
#include "keyboard.h"
#include "shell.h"
#include "timer.h"

/* ---- The screen -----------------------------------------------------------
 * The BIOS leaves the machine in VGA text mode: an 80x25 grid of characters.
 * That grid is not accessed through a function — it is just MEMORY, sitting at
 * address 0xB8000. Write bytes there and characters appear on screen. That is
 * the entire mechanism.
 *
 * Each cell is 2 bytes:
 *     low byte  = the character code
 *     high byte = the colour (low 4 bits foreground, high 4 bits background)
 */
static const size_t VGA_WIDTH  = 80;
static const size_t VGA_HEIGHT = 25;

/* Pack a foreground and background colour into the single byte VGA wants.
 *
 * `bg << 4` shifts the background colour into the upper half of the byte, then
 * `|` merges the two halves. Hardware packs several fields into one byte and
 * you assemble them by hand.
 */
uint8_t vga_entry_colour(enum vga_colour fg, enum vga_colour bg) {
	return fg | (bg << 4);
}

/* Pack a character and its colour into the 16-bit value one cell holds. */
static uint16_t vga_entry(unsigned char ch, uint8_t colour) {
	return (uint16_t) ch | ((uint16_t) colour << 8);
}


/* ---- Talking to hardware --------------------------------------------------
 * outb and inb have moved to io.h so idt.c can use them too. Read that file
 * for the explanation of I/O ports and the inline assembly.
 */

/* ---- The hardware cursor --------------------------------------------------
 * The blinking underscore is drawn by the VGA controller itself, not by us.
 * We just tell it where to put it.
 *
 * The controller has far more internal registers than it has ports, so it uses
 * an index/data pair: write WHICH register you want to 0x3D4, then write the
 * value to 0x3D5. Two ports, dozens of registers. This pattern is everywhere
 * in hardware.
 */
#define VGA_CTRL_PORT 0x3D4   /* "which register do you mean" */
#define VGA_DATA_PORT 0x3D5   /* "here is the value for it"   */

/* Switch the cursor on and set its shape. start and end are scanlines within
 * the character cell (0 is the top). 14 and 15 give the classic underscore.  */
static void terminal_enable_cursor(uint8_t start, uint8_t end) {
	outb(VGA_CTRL_PORT, 0x0A);
	/* Read-modify-write: the top bits of this register mean other things, so
	 * we must preserve them rather than clobbering the whole byte. Bit 5 also
	 * happens to be the cursor-disable flag, and masking with 0xC0 clears it,
	 * which is what actually turns the cursor on.                            */
	outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xC0) | start);

	outb(VGA_CTRL_PORT, 0x0B);
	outb(VGA_DATA_PORT, (inb(VGA_DATA_PORT) & 0xE0) | end);
}

/* Move the cursor to (x, y).
 *
 * The position is a single 16-bit cell offset, but the controller only accepts
 * one byte at a time, so it is split across two registers — 0x0F takes the low
 * byte, 0x0E the high byte. Splitting a wide value across narrow registers is
 * routine once you are talking to real hardware.                             */
static void terminal_move_cursor(size_t x, size_t y) {
	uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);

	outb(VGA_CTRL_PORT, 0x0F);
	outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));          /* low 8 bits  */

	outb(VGA_CTRL_PORT, 0x0E);
	outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));   /* high 8 bits */
}

/* ---- Terminal state ------------------------------------------------------- */
static size_t    terminal_row;
static size_t    terminal_column;
static uint8_t   terminal_colour;
static uint16_t *terminal_buffer;   /* <-- a POINTER. See the note below.     */

/*
 * POINTERS, the one idea to actually stop and absorb.
 *
 * `uint16_t *terminal_buffer` does not hold a character. It holds an ADDRESS —
 * a number saying where in memory something lives. Below we set it to 0xB8000,
 * meaning "the screen starts there". Writing terminal_buffer[5] = x means
 * "go to that address, step forward 5 cells, put x there".
 *
 * The reason C is the language of operating systems is exactly this: it lets
 * you say "treat address 0xB8000 as an array of 16-bit values" and then just
 * do it. Higher-level languages deliberately prevent that. Here it is the
 * whole job.
 */

/* Put one character at an exact position. */
static void terminal_put_at(char ch, uint8_t colour, size_t x, size_t y) {
	terminal_buffer[y * VGA_WIDTH + x] = vga_entry(ch, colour);
}

/* Fill every cell with a space in the current colour. Both initialisation and
 * `clear` want exactly this and nothing else - the difference between them is
 * only whether the hardware cursor is set up as well. */
static void terminal_blank(void) {
	for (size_t y = 0; y < VGA_HEIGHT; y++)
		for (size_t x = 0; x < VGA_WIDTH; x++)
			terminal_put_at(' ', terminal_colour, x, y);
}

void terminal_initialise(void) {
	terminal_row    = 0;
	terminal_column = 0;
	terminal_colour = vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK);
	terminal_buffer = (uint16_t *) 0xB8000;

	terminal_enable_cursor(14, 15);   /* scanlines 14-15: an underscore */

	/* Nothing clears the screen for us. Note how the 2D grid is flattened onto
	 * 1D memory inside terminal_put_at: row y, column x lives at
	 * y * WIDTH + x. */
	terminal_blank();

	terminal_move_cursor(0, 0);
}

void terminal_set_colour(uint8_t colour) {
	terminal_colour = colour;
}

/* Blank the screen and put the cursor back at the top - the same blanking as
 * initialisation, without re-initialising the hardware cursor. */
void terminal_clear(void) {
	terminal_blank();

	terminal_row = 0;
	terminal_column = 0;
	terminal_move_cursor(0, 0);
}

/* Erase the character before the cursor.
 *
 * Note this only steps back within the current row. Backing up over a line
 * wrap would mean remembering how long the previous line was, which the
 * terminal does not track. The shell also refuses to backspace past the start
 * of its buffer, so in practice you cannot reach the edge. */
void terminal_backspace(void) {
	if (terminal_column > 0) {
		terminal_column--;
		terminal_put_at(' ', terminal_colour, terminal_column, terminal_row);
		terminal_move_cursor(terminal_column, terminal_row);
	}
}

/* Move everything on screen up by one row, and blank the bottom row.
 *
 * There is no memmove here. There is no memmove anywhere — we are freestanding,
 * so if we want to move memory we write the loop ourselves.
 *
 * The DIRECTION of the loop matters and is the one place this can go subtly
 * wrong. We are copying each row from the row below it, so we must start at the
 * TOP and work down. Row 0 gets overwritten by row 1 first; by the time we read
 * row 1 as a source we have already finished with it as a destination. Run the
 * loop backwards instead and every row would get filled with the same content,
 * because you would keep copying a row you had just overwritten.
 *
 * This "which end do I start from" question comes up constantly once regions
 * overlap. It is exactly why the C standard has both memcpy and memmove.
 */
static void terminal_scroll(void) {
	for (size_t y = 1; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			/* destination is one row up (y-1), source is this row (y) */
			terminal_buffer[(y - 1) * VGA_WIDTH + x] =
				terminal_buffer[y * VGA_WIDTH + x];
		}
	}

	/* The bottom row still holds a stale copy of what was there before, so
	 * blank it. Using terminal_colour (not a hardcoded grey) means a blank
	 * line inherits whatever colour is currently active. */
	for (size_t x = 0; x < VGA_WIDTH; x++)
		terminal_put_at(' ', terminal_colour, x, VGA_HEIGHT - 1);
}

/* Put one character at the cursor, handling newlines and wrapping ourselves.
 * Note that '\n' is not magic — on a real machine it is just the byte 10, and
 * it only means "new line" because code like this decides it does.
 */
void terminal_putchar(char ch) {
	/* Send to serial as well as VGA. This goes outside the console lock
	 * because serial I/O does not need to be atomic with screen updates —
	 * the host's serial capture will see every character regardless. */
	serial_putchar(ch);

	if (ch == '\n') {
		terminal_column = 0;
		terminal_row++;
	} else {
		terminal_put_at(ch, terminal_colour, terminal_column, terminal_row);
		if (++terminal_column == VGA_WIDTH) {
			terminal_column = 0;
			terminal_row++;
		}
	}

	/* Past the bottom row: scroll instead of wrapping to the top. After
	 * scrolling, the cursor stays on the last row — that row is now blank
	 * and is where the next character belongs.                             */
	if (terminal_row == VGA_HEIGHT) {
		terminal_scroll();
		terminal_row = VGA_HEIGHT - 1;
	}

	/* Keep the hardware cursor in step with where we think we are. */
	terminal_move_cursor(terminal_column, terminal_row);
}

/* Write a string.
 *
 * In C a string is not an object with a length attached. It is just a run of
 * bytes in memory that ends with a zero byte, and `const char *str` is the
 * address of the first one. So "walk forward until you hit 0" is genuinely how
 * you find the end. This is the source of a great many real-world security
 * bugs, and you are seeing the reason for them directly.
 */
void terminal_write(const char *str)
{
	uint32_t flags = console_lock();

	for (size_t i = 0; str[i] != '\0'; i++)
		terminal_putchar(str[i]);

	console_unlock(flags);
}


/* ---- Number printing ------------------------------------------------------
 * There is no itoa, no sprintf, no way to turn 42 into "42". We write it.
 *
 * The algorithm is the one you'd use on paper, and it produces digits in the
 * WRONG ORDER. Repeatedly divide by the base and take the remainder:
 *
 *     1234 % 10 = 4   ->  1234 / 10 = 123
 *      123 % 10 = 3   ->   123 / 10 = 12
 *       12 % 10 = 2   ->    12 / 10 = 1
 *        1 % 10 = 1   ->     1 / 10 = 0   (stop)
 *
 * That gives 4, 3, 2, 1 — least significant digit first. So we collect them
 * into a small buffer and then walk it backwards to print. Every integer
 * formatter ever written does this, including glibc's.
 *
 * `base` makes the same code handle decimal and hexadecimal. 32 chars is
 * comfortably enough: the longest possible output here is a 32-bit number in
 * base 2, which would be 32 digits, and we never go below base 8.
 */
static void terminal_write_uint(uint32_t value, uint32_t base) {
	const char *digits = "0123456789abcdef";
	char buf[32];
	size_t i = 0;

	/* Special case: the loop below produces nothing at all for zero, because
	 * the condition fails immediately. Easy bug to miss — 0 would print as
	 * empty string. */
	if (value == 0) {
		terminal_putchar('0');
		return;
	}

	while (value != 0) {
		buf[i++] = digits[value % base];
		value /= base;
	}

	/* Walk backwards. Note `while (i-- > 0)` rather than `while (i > 0)`:
	 * i-- yields the old value then decrements, so this tests i against 0 and
	 * leaves us with a valid index. Writing `buf[--i]` inside the loop body
	 * would work too; this idiom is just the compact form. */
	while (i-- > 0)
		terminal_putchar(buf[i]);
}

static void terminal_write_int(int32_t value) {
	uint32_t magnitude;

	if (value < 0) {
		terminal_putchar('-');
		/* Careful here. You cannot write -value and be safe: the most
		 * negative int32 (-2147483648) has no positive counterpart, so
		 * negating it overflows, which is undefined behaviour in C — the
		 * compiler is entitled to do anything at all.
		 *
		 * Casting to unsigned FIRST sidesteps it. Unary minus on an unsigned
		 * type is defined as modular arithmetic, so this is well-defined and
		 * gives exactly the magnitude we want. This is a real bug that ships
		 * in real code. */
		magnitude = -(uint32_t)value;
	} else {
		magnitude = (uint32_t)value;
	}

	terminal_write_uint(magnitude, 10);
}

/* ---- kprintf --------------------------------------------------------------
 * Our printf. The k prefix is convention for "kernel version of".
 *
 * The `...` makes it variadic — it takes any number of further arguments. The
 * catch is that C gives us no way to know how many there are or what types
 * they have. printf solves this by trusting the format string: a %d is a
 * promise that an int was passed. Nothing verifies that promise at runtime.
 * Pass the wrong type and you read garbage off the stack. That is why format
 * string bugs are a whole category of security vulnerability.
 *
 * va_list / va_start / va_arg / va_end walk the arguments. They are macros
 * from stdarg.h, which is safe here because it contains no library code.
 *
 * Supported: %d %u %x %s %c %%
 */
void kprintf(const char *fmt, ...) {
	/* Held for the whole call, not per character, so a line comes out whole. */
	uint32_t flags = console_lock();

	va_list args;
	va_start(args, fmt);      /* start reading after `fmt` */

	for (size_t i = 0; fmt[i] != '\0'; i++) {
		if (fmt[i] != '%') {
			terminal_putchar(fmt[i]);
			continue;
		}

		i++;   /* step past the % to look at the specifier */

		switch (fmt[i]) {
		case 'd':
			terminal_write_int(va_arg(args, int32_t));
			break;
		case 'u':
			terminal_write_uint(va_arg(args, uint32_t), 10);
			break;
		case 'x':
			terminal_write_uint(va_arg(args, uint32_t), 16);
			break;
		case 's':
			terminal_write(va_arg(args, const char *));
			break;
		case 'c':
			/* Note `int`, not `char`. Anything smaller than int is promoted
			 * to int when passed through `...`, so asking va_arg for a char
			 * would read the wrong width. A genuine trap. */
			terminal_putchar((char) va_arg(args, int));
			break;
		case '%':
			terminal_putchar('%');
			break;
		case '\0':
			/* String ended on a trailing %. Back up so the outer loop sees
			 * the terminator and stops, instead of running off the end. */
			i--;
			break;
		default:
			/* Unknown specifier: print it literally rather than silently
			 * swallowing it, so mistakes are visible. */
			terminal_putchar('%');
			terminal_putchar(fmt[i]);
			break;
		}
	}

	va_end(args);

	console_unlock(flags);
}

