#ifndef ARTHIC_RAND_H
#define ARTHIC_RAND_H

#include <stdint.h>

void rand_init(void);
uint64_t rand_u64(void);
uint32_t rand_range(uint32_t max);

#endif