/* loader.h - running a program that came from disk. */
#ifndef ARTHIC_LOADER_H
#define ARTHIC_LOADER_H

#include <stdint.h>

/* Where a program is mapped. Chosen above any physical RAM the kernel
 * identity-maps, so these addresses cannot collide with the kernel's own view
 * of memory - they exist only in the page tables and only while a program is
 * loaded. */
#define USER_LOAD_ADDR   0x20000000u
#define USER_STACK_TOP   0x20200000u
#define USER_STACK_SIZE  8192u

/* Read `name` from the filesystem, map it, and run it in ring 3. Returns when
 * the program exits. 0 means it could not be loaded. */
int loader_run(const char *name);

/* Write the built-in demo program to the filesystem as a file. */
int loader_install(const char *name);

#endif
