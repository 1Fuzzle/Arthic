/* terminal.h — screen output, for other files to use. */
#ifndef ARTHIC_TERMINAL_H
#define ARTHIC_TERMINAL_H

void terminal_write(const char *str);
void terminal_putchar(char ch);
void kprintf(const char *fmt, ...);

#endif
