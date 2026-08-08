/* pmm.h — physical memory manager.
 *
 * Hands out physical page frames. One frame is 4096 bytes, because that is the
 * page size the x86 MMU works in and matching it makes paging straightforward
 * later.
 */
#ifndef ARTHIC_PMM_H
#define ARTHIC_PMM_H

#include <stdint.h>
#include "multiboot.h"

#define PAGE_SIZE 4096

void pmm_init(struct multiboot_info *mbi);

/* Returns the physical address of a free frame, or 0 if none remain.
 * 0 is safe as a failure value because physical address 0 is never handed
 * out — the first megabyte is reserved. */
uint32_t pmm_alloc_frame(void);
void     pmm_free_frame(uint32_t addr);

/* Allocate `count` CONSECUTIVE frames and return the address of the first.
 * Needed by anything wanting a contiguous region larger than one page — the
 * heap, and later DMA buffers, which hardware requires to be contiguous in
 * physical memory because devices do not go through the MMU. */
uint32_t pmm_alloc_frames(uint32_t count);

/* Give back `count` consecutive frames starting at `addr`.
 *
 * The counterpart to pmm_alloc_frames, and worth having because every caller
 * that allocated a run had to write the same loop to release it - three of them
 * in the loader alone, on paths taken only when something has gone wrong.
 * Cleanup code that is written out by hand at every error return is cleanup
 * code that eventually gets one of them wrong. */
void     pmm_free_range(uint32_t addr, uint32_t count);

/* One past the highest physical address the memory map mentioned. */
uint32_t pmm_memory_top(void);

uint32_t pmm_total_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_free_frames(void);

#endif
