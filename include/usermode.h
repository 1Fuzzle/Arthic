/* usermode.h — dropping to ring 3 and coming back. */
#ifndef ARTHIC_USERMODE_H
#define ARTHIC_USERMODE_H

#include <stdint.h>

/* Reserve and open up the memory ring 3 needs. Returns 0 if that could not be
 * done, in which case ring 3 is simply unavailable - the `user` command says so
 * rather than jumping into memory the CPU will refuse. */
int usermode_init(void);
void usermode_run(void);    /* enters ring 3; returns when the program exits */
void usermode_exit(void);   /* called from the SYS_EXIT handler */

/* Abandon ring 3 immediately and resume the kernel where usermode_jump was
 * called. Implemented in interrupts.s. Used by SYS_EXIT and by the page fault
 * handler when a program faults. */
void usermode_return(void);

#endif
