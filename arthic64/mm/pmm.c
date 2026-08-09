/* pmm.c — the physical memory manager.
 *
 * THE PROBLEM
 *
 * Up to now every byte Arthic has used was decided at compile time — the stack
 * in boot.s, the GDT and IDT arrays, the shell buffer. All fixed size, all
 * baked into the binary. That does not scale. A real system needs to ask for
 * memory while running and give it back afterwards.
 *
 * The first thing you need is a way to track which physical memory is in use.
 * That is all this file does: it hands out 4096-byte frames and takes them
 * back. No malloc yet — malloc gives you arbitrary sizes and is built on top of
 * this later.
 *
 * THE BITMAP
 *
 * One bit per frame. 1 means used, 0 means free. On a machine with 128 MB that
 * is 32768 frames, so the bitmap costs 4 KB — a thousandth of the memory it
 * describes, which is a good trade.
 *
 * Allocation is a linear scan for a zero bit. That is O(n) and there are far
 * cleverer structures, but this is correct, obviously so, and fast enough that
 * nothing will notice for a long time.
 *
 * WHERE THE BITMAP LIVES
 *
 * A chicken-and-egg problem: we need memory to store the thing that tracks
 * memory. The answer is to place it immediately after the kernel, at the
 * address the linker gave us in `kernel_end`, and then mark that region as used
 * so we never hand it out.
 */

#include "pmm.h"
#include "multiboot.h"
#include "string.h"
#include "terminal.h"

/* Defined by linker.ld. We only ever take its ADDRESS — the value stored there
 * is meaningless. This trick is how C code learns about layout decisions the
 * linker made. */
extern uint64_t kernel_end;

/* Where the kernel starts in physical memory. Set in linker.ld with `. = 1M`.
 * Below this sits the BIOS, video memory and other things that are not ours. */
#define KERNEL_START 0x100000

static uint64_t *bitmap       = 0;
static uint64_t  total_frames = 0;
static uint64_t  used_frames  = 0;

/* 64 bits per word now, so frame N lives in word N/64 at bit N%64. Same idea,
 * one more bit of shift - and a bitmap that covers twice as much per word.
 *
 * The compiler turns / 32 into a shift and % 32 into a mask, so this is as
 * fast as writing the shifts by hand and considerably easier to read. Write it
 * the clear way; let the compiler do the clever way.
 */
static void frame_set(uint64_t frame)
{
	bitmap[frame / 64] |= (1ull << (frame % 64));
}

static void frame_clear(uint64_t frame)
{
	bitmap[frame / 64] &= ~(1ull << (frame % 64));
}

static int frame_test(uint64_t frame)
{
	return (bitmap[frame / 64] & (1ull << (frame % 64))) != 0;
}

/* Mark a physical address range used or free, rounding outward.
 *
 * Rounding direction matters and is not symmetric. When marking something USED
 * we round the start down and the end up, so a partially covered frame counts
 * as used — better to waste a frame than to hand out one containing something
 * important. This is the conservative direction, and in a memory manager
 * conservative is correct.
 */
static void mark_region(uint64_t base, uint64_t length, int used)
{
	uint64_t first = base / PAGE_SIZE;
	uint64_t last  = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;

	for (uint64_t f = first; f < last && f < total_frames; f++) {
		if (used) {
			if (!frame_test(f)) {
				frame_set(f);
				used_frames++;
			}
		} else {
			if (frame_test(f)) {
				frame_clear(f);
				used_frames--;
			}
		}
	}
}

void pmm_init(struct multiboot_info *mbi)
{
	/* Without a memory map we would be guessing, and guessing about which
	 * physical addresses are real RAM is not something to do. */
	if (!(mbi->flags & MULTIBOOT_INFO_MEM_MAP)) {
		kprintf("pmm: no memory map from bootloader, cannot continue\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	/* Pass one: find the highest usable address, so we know how many frames
	 * the bitmap must cover. */
	uint64_t highest = 0;
	uint32_t offset  = 0;

	while (offset < mbi->mmap_length) {
		struct multiboot_mmap_entry *entry =
			(struct multiboot_mmap_entry *)(uint64_t)(mbi->mmap_addr + offset);

		if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
			uint64_t end = entry->addr + entry->len;
			if (end > highest)
				highest = end;
		}

		/* size does not count itself — hence the + 4. */
		offset += entry->size + 4;
	}

	/* No 4 GB ceiling any more - that limit was a property of 32-bit
	 * pointers, and it is gone. We cap at what the boot page tables map
	 * instead, which is a decision we can raise rather than one the
	 * architecture imposes. */
	total_frames = highest / PAGE_SIZE;

	/* Put the bitmap just past the kernel, page-aligned. */
	uint64_t bitmap_addr = ((uint64_t)&kernel_end + PAGE_SIZE - 1)
	                       & ~((uint64_t) PAGE_SIZE - 1);
	bitmap = (uint64_t *) bitmap_addr;

	uint64_t bitmap_bytes = (total_frames + 7) / 8;

	/* Start with EVERYTHING marked used, then free only what the BIOS
	 * explicitly told us is available.
	 *
	 * This direction is deliberate. Start-empty-and-mark-used would treat
	 * any region the map failed to mention as free, and handing out memory
	 * that belongs to a device is a very bad failure. Start-full means the
	 * worst case is wasting memory, which is merely annoying. */
	kmemset(bitmap, 0xFF, bitmap_bytes);
	used_frames = total_frames;

	/* Pass two: free the regions the BIOS says are genuinely RAM. */
	offset = 0;
	while (offset < mbi->mmap_length) {
		struct multiboot_mmap_entry *entry =
			(struct multiboot_mmap_entry *)(uint64_t)(mbi->mmap_addr + offset);

		if (entry->type == MULTIBOOT_MEMORY_AVAILABLE)
			mark_region(entry->addr, entry->len, 0);

		offset += entry->size + 4;
	}

	/* Now claim back everything that is genuinely ours or off limits: the
	 * first megabyte, the kernel itself, and the bitmap. Order matters —
	 * this must happen after the freeing pass, or the map would free the
	 * memory the kernel is running in. */
	mark_region(0, KERNEL_START, 1);
	mark_region(KERNEL_START, (uint64_t)&kernel_end - KERNEL_START, 1);
	mark_region(bitmap_addr, bitmap_bytes, 1);
}

uint64_t pmm_alloc_frame(void)
{
	for (uint64_t f = 0; f < total_frames; f++) {
		if (!frame_test(f)) {
			frame_set(f);
			used_frames++;
			return f * PAGE_SIZE;
		}
	}
	return 0;   /* out of memory */
}

uint64_t pmm_alloc_frames(uint64_t count)
{
	if (count == 0)
		return 0;

	/* Sliding window: walk forward, and every time a used frame appears,
	 * restart the run from the frame after it. One pass, no backtracking.
	 *
	 * This is where a bitmap starts to show its limits — finding a large
	 * contiguous run is O(n) and can fail even when plenty of memory is
	 * free, because it is scattered. That is fragmentation, and it is a real
	 * problem rather than a theoretical one. Buddy allocators exist to make
	 * this cheap. Ours is honest about being simple. */
	uint64_t run_start = 0;
	uint64_t run_length = 0;

	for (uint64_t f = 0; f < total_frames; f++) {
		if (frame_test(f)) {
			run_length = 0;
			run_start = f + 1;
			continue;
		}

		run_length++;

		if (run_length == count) {
			for (uint64_t i = 0; i < count; i++) {
				frame_set(run_start + i);
				used_frames++;
			}
			return run_start * PAGE_SIZE;
		}
	}

	return 0;   /* no contiguous run that large */
}

uint64_t pmm_memory_top(void)
{
	return total_frames * PAGE_SIZE;
}

void pmm_free_frame(uint64_t addr)
{
	uint64_t frame = addr / PAGE_SIZE;

	/* Freeing something already free means a double-free bug somewhere.
	 * Silently ignoring it hides the bug; refusing to decrement at least
	 * keeps the accounting honest. */
	if (frame < total_frames && frame_test(frame)) {
		frame_clear(frame);
		used_frames--;
	}
}

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_used_frames(void)  { return used_frames;  }
uint64_t pmm_free_frames(void)  { return total_frames - used_frames; }
