/* usermode.h — dropping to ring 3 and coming back. */
#ifndef ARTHIC_USERMODE_H
#define ARTHIC_USERMODE_H

void usermode_init(void);
void usermode_run(void);    /* enters ring 3; returns when the program exits */
void usermode_exit(void);   /* called from the SYS_EXIT handler */

#endif
