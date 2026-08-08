/* fake_pmm.c - page frames that are really just malloc.
 *
 * kheap.c asks the frame allocator for one contiguous run of frames and then
 * treats the returned number as an address. That is the entire interface
 * between them, so a fake only has to hand back the address of a suitably
 * aligned lump of memory and remember that it did.
 *
 * The lump is aligned to PAGE_SIZE with posix_memalign because the real
 * allocator only ever returns page-aligned addresses, and code that quietly
 * depends on that should keep working here for the same reason it works in the
 * kernel - not by accident.
 */
#include <stdint.h>
#include <stdlib.h>

#include "support.h"
#include "pmm.h"

static uint8_t *pool;
static uint32_t pool_frames;
static uint32_t frames_taken;
static uint32_t alloc_calls;

void fake_pmm_reset(uint32_t frames_available)
{
	free(pool);
	pool = NULL;

	pool_frames  = frames_available;
	frames_taken = 0;
	alloc_calls  = 0;

	if (frames_available == 0)
		return;

	void *memory = NULL;
	if (posix_memalign(&memory, PAGE_SIZE,
	                   (size_t) frames_available * PAGE_SIZE) != 0)
		abort();                         /* the test cannot run without it */

	pool = memory;
}

void fake_pmm_free_all(void)
{
	free(pool);
	pool = NULL;
	pool_frames = 0;
}

uint32_t fake_pmm_alloc_calls(void)
{
	return alloc_calls;
}

uint32_t pmm_alloc_frames(uint32_t count)
{
	alloc_calls++;

	if (count == 0 || frames_taken + count > pool_frames)
		return 0;                        /* same failure signal as the real one */

	uint8_t *base = pool + (size_t) frames_taken * PAGE_SIZE;
	frames_taken += count;

	/* The cast is the point of building 32-bit: an address has to survive a
	 * round trip through uint32_t, exactly as it does in the kernel. */
	return (uint32_t)(uintptr_t) base;
}

uint32_t pmm_alloc_frame(void)
{
	return pmm_alloc_frames(1);
}

void pmm_free_frame(uint32_t addr)
{
	(void) addr;                         /* the pool is freed wholesale */
}

uint32_t pmm_memory_top(void)   { return (uint32_t) pool_frames * PAGE_SIZE; }
uint32_t pmm_total_frames(void) { return pool_frames; }
uint32_t pmm_used_frames(void)  { return frames_taken; }
uint32_t pmm_free_frames(void)  { return pool_frames - frames_taken; }

void pmm_init(struct multiboot_info *mbi)
{
	(void) mbi;
}
