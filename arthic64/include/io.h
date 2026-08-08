/* io.h — reading and writing x86 I/O ports.
 *
 * Moved here out of kernel.c because idt.c needs them too. This is the whole
 * reason headers exist: one definition, many users.
 *
 * `static inline` in a header means each .c file that includes it gets its own
 * private copy, and the compiler pastes the instruction directly rather than
 * making a function call. Without `static` you would get a duplicate-symbol
 * error at link time from two files defining the same function.
 */
#ifndef ARTHIC_IO_H
#define ARTHIC_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t result;
	__asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
	return result;
}

/* 16-bit port access. Disk data arrives a word at a time, so the ATA driver
 * needs these; nothing else has so far. */
static inline void outw(uint16_t port, uint16_t value) {
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
	uint16_t result;
	__asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
	return result;
}

/* Some old hardware needs a moment between consecutive writes. Writing to
 * unused port 0x80 wastes exactly the right amount of time — an ugly trick,
 * but a universal one. */
static inline void io_wait(void) {
	outb(0x80, 0);
}

#endif
