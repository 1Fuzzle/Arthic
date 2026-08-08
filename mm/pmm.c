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
#include "util.h"
#include "bitmap.h"

/* Defined by linker.ld. We only ever take its ADDRESS — the value stored there
 * is meaningless. This trick is how C code learns about layout decisions the
 * linker made. */
extern uint32_t kernel_end;

/* Where the kernel starts in physical memory. Set in linker.ld with `. = 1M`.
 * Below this sits the BIOS, video memory and other things that are not ours. */
#define KERNEL_START 0x100000

/* One bit per frame, addressed by the shared helpers in bitmap.h - the same
 * ones the filesystem uses for its block bitmap, because "one bit per thing"
 * is the same operation whatever the thing is. */
static uint8_t  *bitmap       = 0;
static uint32_t  total_frames = 0;
static uint32_t  used_frames  = 0;

static void frame_set(uint32_t frame)     { bitmap_set(bitmap, frame);   }
static void frame_clear(uint32_t frame)   { bitmap_clear(bitmap, frame); }
static int  frame_test(uint32_t frame)    { return bitmap_test(bitmap, frame); }

/* Mark a physical address range used or free, rounding outward.
 *
 * Rounding direction matters and is not symmetric. When marking something USED
 * we round the start down and the end up, so a partially covered frame counts
 * as used — better to waste a frame than to hand out one containing something
 * important. This is the conservative direction, and in a memory manager
 * conservative is correct.
 */
static void mark_region(uint32_t base, uint32_t length, int used)
{
	uint32_t first = base / PAGE_SIZE;
	uint32_t last  = KDIV_ROUND_UP(base + length, PAGE_SIZE);

	for (uint32_t f = first; f < last && f < total_frames; f++) {
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
			(struct multiboot_mmap_entry *)(mbi->mmap_addr + offset);

		if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
			uint64_t end = entry->addr + entry->len;
			if (end > highest)
				highest = end;
		}

		/* size does not count itself — hence the + 4. */
		offset += entry->size + 4;
	}

	/* We are a 32-bit kernel: anything past 4 GB is unreachable. */
	if (highest > 0xFFFFFFFFull)
		highest = 0xFFFFFFFFull;

	total_frames = (uint32_t)(highest / PAGE_SIZE);

	/* Put the bitmap just past the kernel, page-aligned. */
	uint32_t bitmap_addr = KALIGN_UP((uint32_t)&kernel_end, PAGE_SIZE);
	bitmap = (uint8_t *) bitmap_addr;

	uint32_t bitmap_bytes = KDIV_ROUND_UP(total_frames, 8);

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
			(struct multiboot_mmap_entry *)(mbi->mmap_addr + offset);

		if (entry->type == MULTIBOOT_MEMORY_AVAILABLE
		    && entry->addr < 0xFFFFFFFFull) {
			uint32_t base = (uint32_t) entry->addr;
			uint32_t len  = (entry->addr + entry->len > 0xFFFFFFFFull)
			                ? (0xFFFFFFFFu - base)
			                : (uint32_t) entry->len;
			mark_region(base, len, 0);
		}

		offset += entry->size + 4;
	}

	/* Now claim back everything that is genuinely ours or off limits: the
	 * first megabyte, the kernel itself, and the bitmap. Order matters —
	 * this must happen after the freeing pass, or the map would free the
	 * memory the kernel is running in. */
	mark_region(0, KERNEL_START, 1);
	mark_region(KERNEL_START, (uint32_t)&kernel_end - KERNEL_START, 1);
	mark_region(bitmap_addr, bitmap_bytes, 1);
}

uint32_t pmm_alloc_frame(void)
{
	for (uint32_t f = 0; f < total_frames; f++) {
		if (!frame_test(f)) {
			frame_set(f);
			used_frames++;
			return f * PAGE_SIZE;
		}
	}
	return 0;   /* out of memory */
}

uint32_t pmm_alloc_frames(uint32_t count)
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
	uint32_t run_start = 0;
	uint32_t run_length = 0;

	for (uint32_t f = 0; f < total_frames; f++) {
		if (frame_test(f)) {
			run_length = 0;
			run_start = f + 1;
			continue;
		}

		run_length++;

		if (run_length == count) {
			for (uint32_t i = 0; i < count; i++) {
				frame_set(run_start + i);
				used_frames++;
			}
			return run_start * PAGE_SIZE;
		}
	}

	return 0;   /* no contiguous run that large */
}

void pmm_free_range(uint32_t addr, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		pmm_free_frame(addr + i * PAGE_SIZE);
}

uint32_t pmm_memory_top(void)
{
	return total_frames * PAGE_SIZE;
}

void pmm_free_frame(uint32_t addr)
{
	uint32_t frame = addr / PAGE_SIZE;

	/* Freeing something already free means a double-free bug somewhere.
	 * Silently ignoring it hides the bug; refusing to decrement at least
	 * keeps the accounting honest. */
	if (frame < total_frames && frame_test(frame)) {
		frame_clear(frame);
		used_frames--;
	}
}

uint32_t pmm_total_frames(void) { return total_frames; }
uint32_t pmm_used_frames(void)  { return used_frames;  }
uint32_t pmm_free_frames(void)  { return total_frames - used_frames; }
