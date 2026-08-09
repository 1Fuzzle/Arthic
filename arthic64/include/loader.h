/* loader.h - running a program that came from disk, 64-bit. */
#ifndef ARTHIC_LOADER_H
#define ARTHIC_LOADER_H

#include <stdint.h>

/* Well above the identity-mapped low gigabyte the kernel uses, so a
 * process's addresses cannot collide with the kernel's own view of memory -
 * they exist only in that process's PML4 and mean nothing outside it. */
#define USER_LOAD_ADDR   0x0000000040000000ull
#define USER_STACK_TOP   0x0000000040200000ull
#define USER_STACK_SIZE  8192ull

int loader_run(const char *name);

#endif
