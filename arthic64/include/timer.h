/* timer.h — the PIT tick counter. */
#ifndef ARTHIC_TIMER_H
#define ARTHIC_TIMER_H

#include <stdint.h>

void     timer_install(void);
uint32_t timer_get_ticks(void);

#endif
