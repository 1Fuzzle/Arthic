/* loader.h - running a program that came from disk. */
#ifndef ARTHIC_LOADER_H
#define ARTHIC_LOADER_H
#define ASLR_STACK_SLACK 0x10000u

#include <stdint.h>

/* Where a program is mapped. Chosen above any physical RAM the kernel
 * identity-maps, so these addresses cannot collide with the kernel's own view
 * of memory - they exist only in the page tables and only while a program is
 * loaded. */
#define USER_LOAD_ADDR   0x20000000u
#define USER_STACK_TOP   0x20200000u
#define USER_STACK_SIZE  8192u

/* ASLR: each program's stack top is randomized within this many bytes below
 * USER_STACK_TOP, page-aligned. USER_STACK_TOP itself becomes the ceiling of
 * the arena rather than a fixed address any program actually uses. */
#define ASLR_STACK_SLACK 0x10000u

/* One page holding the argument string, mapped read-only. A fixed address
 * rather than something passed in a register: the program has no startup code
 * to unpack arguments, so an agreed location is the simplest contract that
 * works. Real systems put argv on the stack and let the C runtime sort it out,
 * which needs a C runtime. */
#define USER_ARGS_ADDR   0x20100000u
#define USER_ARGS_MAX    256u

/* Read `name` from the filesystem, map it, and run it in ring 3. Returns when
 * the program exits. 0 means it could not be loaded. */
int loader_run(const char *name, const char *args);

/* Write the built-in demo program to the filesystem as a file. */
int loader_install(const char *name);

#endif
