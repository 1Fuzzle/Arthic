/* kernel.c — Arthic
 *
 * This is freestanding C. That word matters: normally C programs sit on top of
 * an operating system that provides printf, malloc, files, and so on. Here we
 * ARE the operating system, so none of that exists. There is no stdio.h. If we
 * want to put a character on the screen, we have to write to the hardware
 * ourselves.
 *
 * What we get instead is: the C language itself, and raw memory.
 */

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

/* VGA's 16 available colours, in the order the hardware numbers them. */
enum vga_colour {
	VGA_BLACK = 0,  VGA_BLUE = 1,       VGA_GREEN = 2,       VGA_CYAN = 3,
	VGA_RED = 4,    VGA_MAGENTA = 5,    VGA_BROWN = 6,       VGA_LIGHT_GREY = 7,
	VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
	VGA_LIGHT_RED = 12, VGA_PINK = 13,  VGA_YELLOW = 14,     VGA_WHITE = 15,
};

/* Pack a foreground and background colour into the single byte VGA wants.
 *
 * `bg << 4` shifts the background colour left by 4 bits, moving it into the
 * upper half of the byte. Then `|` merges the two halves together. Bit
 * shifting like this is everywhere in systems code — hardware packs several
 * fields into one byte and you assemble them by hand.
 */
static uint8_t vga_entry_colour(enum vga_colour fg, enum vga_colour bg) {
	return fg | (bg << 4);
}

/* Pack a character and its colour into the 16-bit value one cell holds. */
static uint16_t vga_entry(unsigned char ch, uint8_t colour) {
	return (uint16_t) ch | ((uint16_t) colour << 8);
}


/* ---- Talking to hardware: I/O ports ---------------------------------------
 * Up to now the only hardware we have touched is the screen, and we did it by
 * writing to memory at 0xB8000. Most devices are not reachable that way.
 *
 * x86 has a SECOND address space, entirely separate from memory, called the
 * I/O port space. It has its own 65536 addresses and its own two instructions —
 * `out` to write and `in` to read. Port 0x3D4 has nothing whatsoever to do with
 * memory address 0x3D4. They are unrelated worlds that happen to both use
 * numbers.
 *
 * C has no way to express `out`, so we drop into assembly. That is what
 * __asm__ does: it hands the compiler literal instructions to emit.
 *
 * Reading the constraint syntax, which is genuinely ugly:
 *   : : "a"(value), "Nd"(port)   — the empty first slot means no outputs.
 *                                  "a" = put `value` in register al/ax/eax.
 *                                  "Nd" = put `port` in dx, or inline it as a
 *                                  constant if it is small enough.
 *   volatile                     — do not optimise this away or reorder it.
 *                                  The compiler cannot see that it has an
 *                                  effect, because the effect happens outside
 *                                  the CPU entirely.
 *
 * You will write these two functions once and then use them forever. Every
 * driver from here on — keyboard, timer, disk — goes through them.
 */
static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t result;
	__asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
	return result;
}

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

void terminal_initialise(void) {
	terminal_row    = 0;
	terminal_column = 0;
	terminal_colour = vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK);
	terminal_buffer = (uint16_t *) 0xB8000;

	terminal_enable_cursor(14, 15);   /* scanlines 14-15: an underscore */

	/* Blank every cell. Nothing clears the screen for us. */
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			/* The screen is a 2D grid but memory is a 1D line, so we flatten
			 * the coordinates: row y, column x lives at y * WIDTH + x.       */
			terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_colour);
		}
	}

	terminal_move_cursor(0, 0);
}

void terminal_set_colour(uint8_t colour) {
	terminal_colour = colour;
}

/* Put one character at an exact position. */
static void terminal_put_at(char ch, uint8_t colour, size_t x, size_t y) {
	terminal_buffer[y * VGA_WIDTH + x] = vga_entry(ch, colour);
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
	for (size_t x = 0; x < VGA_WIDTH; x++) {
		terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
			vga_entry(' ', terminal_colour);
	}
}

/* Put one character at the cursor, handling newlines and wrapping ourselves.
 * Note that '\n' is not magic — on a real machine it is just the byte 10, and
 * it only means "new line" because code like this decides it does.
 */
void terminal_putchar(char ch) {
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
void terminal_write(const char *str) {
	for (size_t i = 0; str[i] != '\0'; i++)
		terminal_putchar(str[i]);
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
}

/* ---- Entry point ----------------------------------------------------------
 * boot.s calls this. Note it never returns — an OS kernel has nothing to
 * return to.
 */
void kernel_main(void) {
	terminal_initialise();

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_CYAN, VGA_BLACK));
	terminal_write("  _   _   _   _   _   _\n");
	terminal_write(" / \\ / \\ / \\ / \\ / \\ / \\\n");
	terminal_write("( A | r | t | h | i | c )\n");
	terminal_write(" \\_/ \\_/ \\_/ \\_/ \\_/ \\_/\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK));
	terminal_write("Arthic kernel v0.5\n");
	terminal_write("Booted in 32-bit protected mode.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_DARK_GREY, VGA_BLACK));
	terminal_write("No scheduler. No memory manager. No drivers.\n");
	terminal_write("Just this. Everything else is yours to add.\n\n");

	/* Print enough lines to push the banner off the top, proving the scroll
	 * works.
	 *
	 * Note `char line[]` and not `const char *line`. That difference is real:
	 * a char array is our own writable COPY of those bytes, so line[5] = c
	 * edits it. Declared as a pointer it would point at read-only memory and
	 * writing through it would be undefined behaviour. Same-looking text,
	 * completely different thing.                                          */
	/* Replace GRUB's borrowed descriptor table with our own before doing
	 * anything else. Nothing visible happens — that is the point. */
	gdt_install();

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREEN, VGA_BLACK));
	kprintf("GDT installed: 5 entries, flat model, ring 0 + ring 3 ready.\n");
	kprintf("kprintf is alive.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK));
	kprintf("  decimal      %d\n", 1234);
	kprintf("  negative     %d\n", -4321);
	kprintf("  zero         %d\n", 0);
	kprintf("  int minimum  %d\n", (int32_t) 0x80000000);
	kprintf("  unsigned     %u\n", 4294967295u);
	kprintf("  hex          0x%x\n", 0xDEADBEEFu);
	kprintf("  string       %s\n", "Arthic");
	kprintf("  character    %c\n", 'A');
	kprintf("  percent      100%%\n");
	kprintf("  mixed        %s v0.%d at 0x%x\n", "kernel", 4, 0xB8000u);

	terminal_set_colour(vga_entry_colour(VGA_WHITE, VGA_BLACK));
	kprintf("\narthic> ");

	/* Fall off the end and boot.s parks the CPU. */
}
