/* test_pmm.c - mm/pmm.c.
 *
 * The frame allocator is the one module where a bug is unrecoverable: hand out
 * a frame that belongs to the kernel image, or to a device, and the failure
 * appears somewhere else entirely, long afterwards. In QEMU the only visible
 * evidence is a number on the `mem` line, which tells you nothing about WHICH
 * frames are considered free.
 *
 * Here the real pmm.c runs against a memory map the test wrote by hand, so
 * every question can be asked directly: is the first megabyte protected, is
 * the kernel image protected, is the bitmap itself protected, and does the
 * accounting still add up after several thousand allocations.
 *
 * The one piece of scaffolding is the `kernel_end` symbol, which normally
 * comes from linker.ld - see tests/support/kernel_end.c.
 */
#include <stdlib.h>
#include <string.h>

#include "arthictest.h"

#include "support/support.h"

#include "multiboot.h"
#include "pmm.h"

#define MEGABYTE (1024u * 1024u)
#define MACHINE_BYTES (256u * MEGABYTE)

extern uint32_t kernel_end;

/* A memory map shaped like a real PC's: usable memory below the BIOS area, a
 * reserved hole, then everything above 1 MB. The hole is the interesting part
 * - nothing in it may ever be handed out. */
static struct multiboot_mmap_entry mmap[3];
static struct multiboot_info       mbi;

static void machine_with_memory(void)
{
	memset(mmap, 0, sizeof(mmap));

	/* `size` excludes itself, which is why pmm.c advances by size + 4. Getting
	 * this wrong here would make the tests agree with a broken walk. */
	for (unsigned i = 0; i < 3; i++)
		mmap[i].size = sizeof(struct multiboot_mmap_entry) - 4;

	mmap[0].addr = 0;
	mmap[0].len  = 640 * 1024;
	mmap[0].type = MULTIBOOT_MEMORY_AVAILABLE;

	mmap[1].addr = 640 * 1024;                  /* BIOS, video, ROM */
	mmap[1].len  = MEGABYTE - 640 * 1024;
	mmap[1].type = 2;                           /* reserved */

	mmap[2].addr = MEGABYTE;
	mmap[2].len  = MACHINE_BYTES - MEGABYTE;
	mmap[2].type = MULTIBOOT_MEMORY_AVAILABLE;

	memset(&mbi, 0, sizeof(mbi));
	mbi.flags       = MULTIBOOT_INFO_MEM_MAP;
	mbi.mmap_addr   = (uint32_t)(uintptr_t) mmap;
	mbi.mmap_length = sizeof(mmap);

	console_reset();
	pmm_init(&mbi);
}

/* Where the bitmap ends up, computed the same way pmm.c computes it. Anything
 * below this is either the kernel or the bitmap, and must never be free. */
static uint32_t reserved_top(void)
{
	uint32_t bitmap_addr = ((uint32_t)(uintptr_t) &kernel_end + 4095u) & ~4095u;
	uint32_t bitmap_bytes = ((MACHINE_BYTES / PAGE_SIZE) + 7) / 8;

	return bitmap_addr + bitmap_bytes;
}

TEST(init_counts_frames_from_the_highest_usable_address)
{
	machine_with_memory();

	CHECK_EQ(pmm_total_frames(), MACHINE_BYTES / PAGE_SIZE);
	CHECK_EQ(pmm_memory_top(), MACHINE_BYTES);
}

TEST(accounting_always_adds_up)
{
	machine_with_memory();

	CHECK_EQ(pmm_used_frames() + pmm_free_frames(), pmm_total_frames());

	/* Everything below the end of the bitmap is spoken for - the first
	 * megabyte, the kernel image, the bitmap itself - and everything above
	 * it is free, because the map said all of it is RAM. Anything else means
	 * the "start with everything used, then free what the BIOS listed" pass
	 * freed too much or too little.
	 *
	 * (In the kernel that reserved part is a megabyte or two. Here it is
	 * wherever the host loader put the fake kernel_end, which is why this is
	 * computed rather than written down.) */
	uint32_t reserved_frames = (reserved_top() + PAGE_SIZE - 1) / PAGE_SIZE;

	CHECK_EQ(pmm_used_frames(), reserved_frames);
	CHECK_EQ(pmm_free_frames(), pmm_total_frames() - reserved_frames);
}

TEST(reserved_regions_are_never_handed_out)
{
	machine_with_memory();

	uint32_t limit = reserved_top();

	/* Thousands of frames, so this walks well past the kernel image rather
	 * than just checking the first answer. */
	for (int i = 0; i < 4096; i++) {
		uint32_t frame = pmm_alloc_frame();

		CHECK(frame != 0);
		if (frame == 0)
			break;

		/* Three separate promises: never the first megabyte (BIOS and video
		 * memory live there), never the kernel image, never the bitmap. */
		CHECK(frame >= MEGABYTE);
		CHECK(frame >= limit || frame + PAGE_SIZE <= (uint32_t)(uintptr_t) &kernel_end);
		CHECK_EQ(frame % PAGE_SIZE, 0);
	}
}

TEST(a_frame_is_only_handed_out_once)
{
	machine_with_memory();

	uint32_t previous = 0;

	/* Allocation walks the bitmap in order, so each address must be strictly
	 * greater than the last. Any repeat means a bit was not set. */
	for (int i = 0; i < 1000; i++) {
		uint32_t frame = pmm_alloc_frame();
		CHECK(frame > previous);
		previous = frame;
	}
}

TEST(allocating_and_freeing_returns_the_frame_to_the_pool)
{
	machine_with_memory();

	uint32_t before = pmm_free_frames();

	uint32_t frame = pmm_alloc_frame();
	CHECK_EQ(pmm_free_frames(), before - 1);

	pmm_free_frame(frame);
	CHECK_EQ(pmm_free_frames(), before);

	/* First fit, so the very next allocation takes it straight back. */
	CHECK_EQ(pmm_alloc_frame(), frame);
}

TEST(freeing_a_free_frame_does_not_corrupt_the_count)
{
	machine_with_memory();

	uint32_t frame = pmm_alloc_frame();
	pmm_free_frame(frame);

	uint32_t after_first_free = pmm_free_frames();

	/* A double free is a bug in the caller. The allocator's job is to not
	 * make it worse by inventing a frame that is now free twice. */
	pmm_free_frame(frame);
	pmm_free_frame(frame);

	CHECK_EQ(pmm_free_frames(), after_first_free);
}

TEST(freeing_an_address_beyond_memory_is_ignored)
{
	machine_with_memory();

	uint32_t before = pmm_free_frames();

	pmm_free_frame(MACHINE_BYTES + PAGE_SIZE);
	pmm_free_frame(0xFFFFF000u);

	CHECK_EQ(pmm_free_frames(), before);
}

TEST(freeing_an_unaligned_address_frees_the_frame_containing_it)
{
	machine_with_memory();

	uint32_t frame = pmm_alloc_frame();
	uint32_t before = pmm_free_frames();

	/* Integer division truncates, so an address part-way into a frame names
	 * that frame. Worth pinning down: it means a caller passing a pointer
	 * into the middle of a page still frees the right thing. */
	pmm_free_frame(frame + 100);

	CHECK_EQ(pmm_free_frames(), before + 1);
}

TEST(a_contiguous_run_is_contiguous_and_marked_used)
{
	machine_with_memory();

	uint32_t before = pmm_free_frames();
	uint32_t base = pmm_alloc_frames(16);

	CHECK(base != 0);
	CHECK_EQ(base % PAGE_SIZE, 0);
	CHECK_EQ(pmm_free_frames(), before - 16);

	/* The next single frame must come from after the run, not from inside
	 * it - the whole run has to be marked, not just its first frame. */
	uint32_t next = pmm_alloc_frame();
	CHECK(next >= base + 16 * PAGE_SIZE);
}

TEST(a_run_skips_over_used_frames_rather_than_straddling_them)
{
	machine_with_memory();

	/* Poke a hole: take four frames and give back only the middle two, so no
	 * run of four can start there. The sliding window has to restart past the
	 * obstruction rather than counting through it. */
	uint32_t a = pmm_alloc_frame();
	uint32_t b = pmm_alloc_frame();
	uint32_t c = pmm_alloc_frame();
	uint32_t d = pmm_alloc_frame();
	(void) a;
	(void) d;

	pmm_free_frame(b);
	pmm_free_frame(c);

	uint32_t run = pmm_alloc_frames(4);

	CHECK(run != 0);
	CHECK(run > d);                  /* not the two-frame hole at b..c */
}

TEST(a_run_of_zero_is_refused)
{
	machine_with_memory();

	uint32_t before = pmm_free_frames();

	CHECK_EQ(pmm_alloc_frames(0), 0);
	CHECK_EQ(pmm_free_frames(), before);
}

TEST(a_run_larger_than_memory_fails_without_taking_anything)
{
	machine_with_memory();

	uint32_t before = pmm_free_frames();

	CHECK_EQ(pmm_alloc_frames(pmm_total_frames() + 1), 0);

	/* The failure must be free of side effects: a partially marked run would
	 * leak frames that nothing can ever hand out again. */
	CHECK_EQ(pmm_free_frames(), before);
}

TEST(exhausting_memory_returns_zero)
{
	/* A deliberately tiny machine, so running it dry takes a moment rather
	 * than a minute. 2 MB is one megabyte of BIOS plus one usable. */
	memset(mmap, 0, sizeof(mmap));
	mmap[0].size = sizeof(struct multiboot_mmap_entry) - 4;
	mmap[0].addr = 0;
	mmap[0].len  = 2 * MEGABYTE;
	mmap[0].type = MULTIBOOT_MEMORY_AVAILABLE;

	memset(&mbi, 0, sizeof(mbi));
	mbi.flags       = MULTIBOOT_INFO_MEM_MAP;
	mbi.mmap_addr   = (uint32_t)(uintptr_t) mmap;
	mbi.mmap_length = mmap[0].size + 4;

	pmm_init(&mbi);

	/* Everything above 1 MB on this machine is either kernel or bitmap as far
	 * as pmm is concerned, because the fake kernel_end sits well past 2 MB.
	 * So there is nothing to hand out at all, and the allocator must say so
	 * rather than returning something. */
	CHECK_EQ(pmm_alloc_frame(), 0);
	CHECK_EQ(pmm_alloc_frames(2), 0);

	machine_with_memory();               /* leave a sane state behind */
}

int main(void)
{
	RUN(init_counts_frames_from_the_highest_usable_address);
	RUN(accounting_always_adds_up);
	RUN(reserved_regions_are_never_handed_out);
	RUN(a_frame_is_only_handed_out_once);
	RUN(allocating_and_freeing_returns_the_frame_to_the_pool);
	RUN(freeing_a_free_frame_does_not_corrupt_the_count);
	RUN(freeing_an_address_beyond_memory_is_ignored);
	RUN(freeing_an_unaligned_address_frees_the_frame_containing_it);
	RUN(a_contiguous_run_is_contiguous_and_marked_used);
	RUN(a_run_skips_over_used_frames_rather_than_straddling_them);
	RUN(a_run_of_zero_is_refused);
	RUN(a_run_larger_than_memory_fails_without_taking_anything);
	RUN(exhausting_memory_returns_zero);

	return test_report("pmm");
}
