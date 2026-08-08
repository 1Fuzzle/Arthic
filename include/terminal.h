/* terminal.h — screen output, for other files to use. */
#ifndef ARTHIC_TERMINAL_H
#define ARTHIC_TERMINAL_H

#include <stdint.h>

/* VGA's 16 available colours, in the order the hardware numbers them. */
enum vga_colour {
	VGA_BLACK = 0,  VGA_BLUE = 1,       VGA_GREEN = 2,       VGA_CYAN = 3,
	VGA_RED = 4,    VGA_MAGENTA = 5,    VGA_BROWN = 6,       VGA_LIGHT_GREY = 7,
	VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10, VGA_LIGHT_CYAN = 11,
	VGA_LIGHT_RED = 12, VGA_PINK = 13,  VGA_YELLOW = 14,     VGA_WHITE = 15,
};

/* Pack a foreground and background colour into the single byte VGA wants.
 * Defined in drivers/terminal.c. */
uint8_t vga_entry_colour(enum vga_colour fg, enum vga_colour bg);

void terminal_initialise(void);
void terminal_set_colour(uint8_t colour);
void terminal_write(const char *str);
void terminal_putchar(char ch);
void kprintf(const char *fmt, ...);
void terminal_clear(void);
void terminal_backspace(void);

/* Report an unrecoverable failure and stop the machine. Never returns.
 *
 * `_Noreturn` tells the compiler control never comes back, so a caller does
 * not need a `return` after it and no "variable may be used uninitialised"
 * warning appears for code past the call. It is a promise the function keeps
 * with the `for (;;)` in its body.
 *
 * Halting is the right answer only when carrying on would run the kernel in a
 * state it cannot describe - no heap, no scheduler, no memory map. Anything
 * recoverable should return a failure to its caller instead. */
_Noreturn void kpanic(const char *reason);

#endif
