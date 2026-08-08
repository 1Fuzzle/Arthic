/* test_kheap.c - mm/kheap.c.
 *
 * The heap is the module with the most to go wrong and the least visible
 * evidence when it does. A missed coalesce or a bad split does not crash: it
 * quietly makes a later, unrelated allocation fail, or hands two callers the
 * same memory. Booting the kernel and running `heaptest` proves it works for
 * one particular sequence. These tests go after the states that sequence never
 * reaches.
 *
 * The real mm/kheap.c is linked in unmodified. Underneath it is a fake frame
 * allocator handing out plain malloc'd memory, which the heap cannot tell from
 * physical RAM - it only ever gets an address and a promise that the run is
 * contiguous.
 */
#include "arthictest.h"

#include "support/support.h"

#include "kheap.h"
#include "pmm.h"
#include "string.h"

#define HEAP_FRAMES 256                          /* must match kheap.c */
#define HEAP_SIZE   (HEAP_FRAMES * PAGE_SIZE)

/* kheap.c keeps its state in file statics, so every test starts by handing it
 * a brand new pool. The old one is abandoned - these are short-lived test
 * processes and leaking a megabyte is cheaper than tracking it. */
static void heap_fresh(void)
{
	console_reset();
	fake_pmm_reset(HEAP_FRAMES);
	kheap_init();
}

static uint32_t used_bytes(void)
{
	uint32_t used = 0;
	kheap_stats(NULL, &used, NULL);
	return used;
}

static uint32_t block_count(void)
{
	uint32_t blocks = 0;
	kheap_stats(NULL, NULL, &blocks);
	return blocks;
}

/* MUST run first: it is the only chance to see the heap before kheap_init has
 * ever succeeded, and there is no way back to that state afterwards. */
TEST(malloc_before_init_returns_null)
{
	CHECK(kmalloc(16) == NULL);
}

TEST(init_reports_failure_when_no_frames_are_available)
{
	console_reset();
	fake_pmm_reset(0);                   /* an empty pool: allocation fails */

	kheap_init();

	CHECK(console_contains("kheap: could not reserve"));
	CHECK(kmalloc(16) == NULL);
}

TEST(init_claims_one_megabyte_as_a_single_free_block)
{
	heap_fresh();

	uint32_t total = 0, used = 0, blocks = 0;
	kheap_stats(&total, &used, &blocks);

	CHECK_EQ(total, HEAP_SIZE);
	CHECK_EQ(used, 0);
	CHECK_EQ(blocks, 1);
	CHECK_EQ(fake_pmm_alloc_calls(), 1);   /* one contiguous run, not 256 */
}

TEST(malloc_returns_usable_aligned_memory)
{
	heap_fresh();

	unsigned char *p = kmalloc(100);
	CHECK(p != NULL);

	/* Eight-byte alignment is promised to every caller, and something will
	 * eventually depend on it without saying so. */
	CHECK_EQ((uintptr_t) p % 8, 0);

	/* Writing the whole region catches a block handed out smaller than
	 * asked for, at least under a sanitiser or a guard page. */
	kmemset(p, 0x5A, 100);
	for (int i = 0; i < 100; i++)
		CHECK_EQ(p[i], 0x5A);
}

TEST(malloc_rounds_sizes_up_to_the_alignment)
{
	heap_fresh();

	kmalloc(1);

	/* 1 byte becomes 8, plus a 24-byte header. Asserting the exact number is
	 * deliberate: it is the only way to notice the header growing, which
	 * silently changes how much of the heap is overhead. */
	CHECK_EQ(used_bytes(), 32);
}

TEST(malloc_of_zero_returns_null)
{
	heap_fresh();

	CHECK(kmalloc(0) == NULL);
	CHECK_EQ(used_bytes(), 0);
}

TEST(allocations_do_not_overlap)
{
	heap_fresh();

	char *a = kmalloc(64);
	char *b = kmalloc(64);
	char *c = kmalloc(64);

	CHECK(a && b && c);

	kmemset(a, 'a', 64);
	kmemset(b, 'b', 64);
	kmemset(c, 'c', 64);

	/* If any two regions overlapped, the later kmemset would have trampled
	 * the earlier one. */
	CHECK_EQ(a[63], 'a');
	CHECK_EQ(b[63], 'b');
	CHECK_EQ(c[63], 'c');

	CHECK(b >= a + 64);
	CHECK(c >= b + 64);
}

TEST(splitting_creates_a_free_remainder)
{
	heap_fresh();

	kmalloc(64);

	/* One allocated block plus the remainder of the heap. Without splitting
	 * the first kmalloc would have swallowed the entire megabyte. */
	CHECK_EQ(block_count(), 2);
	CHECK(used_bytes() < HEAP_SIZE);
}

TEST(a_block_too_small_to_split_is_handed_over_whole)
{
	heap_fresh();

	/* Ask for so nearly everything that the leftover could not hold a header
	 * plus a payload. The heap must hand the block over whole rather than
	 * carve off a remainder too small to ever be allocated - that would be a
	 * list node to step past forever, holding memory nothing can use. */
	void *everything = kmalloc(HEAP_SIZE - 48);

	CHECK(everything != NULL);
	CHECK_EQ(block_count(), 1);

	/* The whole heap is charged to the caller, header included: the few bytes
	 * that were too small to split are inside the block now. */
	CHECK_EQ(used_bytes(), HEAP_SIZE);
}

TEST(free_returns_the_space_and_it_can_be_reused)
{
	heap_fresh();

	void *first = kmalloc(128);
	CHECK(used_bytes() > 0);

	kfree(first);
	CHECK_EQ(used_bytes(), 0);

	/* First fit means the same request lands in the same place. */
	void *second = kmalloc(128);
	CHECK(second == first);
}

TEST(freeing_null_is_a_no_op)
{
	heap_fresh();

	kfree(NULL);

	CHECK_EQ(used_bytes(), 0);
	CHECK_EQ(console_text()[0], '\0');   /* and says nothing about it */
}

TEST(adjacent_free_blocks_are_merged)
{
	heap_fresh();

	void *a = kmalloc(64);
	void *b = kmalloc(64);
	void *c = kmalloc(64);
	CHECK_EQ(block_count(), 4);          /* three in use, one remainder */

	/* Free the outer two first so the middle one, freed last, has a free
	 * neighbour on each side. That exercises both directions of the merge in
	 * a single kfree - and getting only one of them would leave the heap
	 * fragmented in a way nothing else would report. */
	kfree(a);
	kfree(c);
	kfree(b);

	CHECK_EQ(block_count(), 1);
	CHECK_EQ(used_bytes(), 0);

	/* The real proof: a request larger than any individual piece now
	 * succeeds, which is the entire reason merging exists. */
	void *whole = kmalloc(HEAP_SIZE - 1024);
	CHECK(whole != NULL);
}

TEST(fragmentation_without_merging_would_be_visible_here)
{
	heap_fresh();

	void *a = kmalloc(64);
	void *b = kmalloc(64);
	void *c = kmalloc(64);
	(void) b;

	kfree(a);
	kfree(c);

	/* a and c are not adjacent, so freeing both cannot produce one block. c
	 * does merge with the tail remainder behind it, leaving three: the hole
	 * where a was, b still in use, and c joined to the rest of the heap.
	 * Merging must reach its neighbours and stop there. */
	CHECK_EQ(block_count(), 3);

	/* The hole where a was is genuinely still a hole - a 64-byte request
	 * lands back in it rather than at the far end of the heap. */
	CHECK(kmalloc(64) == a);
}

TEST(double_free_is_refused_and_reported)
{
	heap_fresh();

	void *p = kmalloc(32);
	kfree(p);

	console_reset();
	kfree(p);

	CHECK(console_contains("double free"));

	/* The heap must still be intact: one merged block, nothing used. A
	 * double free that corrupted the list would show up as either. */
	CHECK_EQ(block_count(), 1);
	CHECK_EQ(used_bytes(), 0);
}

TEST(freeing_a_pointer_the_heap_never_gave_out_is_refused)
{
	heap_fresh();

	char stack_object[64];

	console_reset();
	kfree(stack_object + 32);

	CHECK(console_contains("bad or corrupted block"));
	CHECK_EQ(block_count(), 1);
}

TEST(a_trampled_header_is_caught_by_the_magic_number)
{
	heap_fresh();

	unsigned char *victim = kmalloc(64);

	/* Simulate an overflow from the block before: 24 bytes back from the
	 * payload is this block's header, and the first field is the magic. */
	kmemset(victim - 24, 0, 4);

	console_reset();
	kfree(victim);

	CHECK(console_contains("bad or corrupted block"));
}

TEST(exhausting_the_heap_returns_null_rather_than_overrunning)
{
	heap_fresh();

	CHECK(kmalloc(HEAP_SIZE) == NULL);           /* no room for the header */
	CHECK(kmalloc(HEAP_SIZE * 4) == NULL);
	CHECK_EQ(used_bytes(), 0);                   /* a failure costs nothing */

	/* And the heap still works afterwards - a failed request must not leave
	 * the free list in a half-modified state. */
	CHECK(kmalloc(64) != NULL);
}

TEST(many_allocations_and_frees_leave_the_heap_whole)
{
	heap_fresh();

	/* The pattern that kills an allocator without coalescing: churn through
	 * a lot of small blocks, then ask for one big one. */
	for (int round = 0; round < 200; round++) {
		void *p = kmalloc(1024);
		CHECK(p != NULL);
		kfree(p);
	}

	CHECK_EQ(block_count(), 1);
	CHECK_EQ(used_bytes(), 0);
	CHECK(kmalloc(HEAP_SIZE - 1024) != NULL);
}

TEST(stats_tolerate_null_arguments)
{
	heap_fresh();

	/* Callers ask for only the number they want; the others are NULL. */
	kheap_stats(NULL, NULL, NULL);
}

int main(void)
{
	RUN(malloc_before_init_returns_null);
	RUN(init_reports_failure_when_no_frames_are_available);
	RUN(init_claims_one_megabyte_as_a_single_free_block);
	RUN(malloc_returns_usable_aligned_memory);
	RUN(malloc_rounds_sizes_up_to_the_alignment);
	RUN(malloc_of_zero_returns_null);
	RUN(allocations_do_not_overlap);
	RUN(splitting_creates_a_free_remainder);
	RUN(a_block_too_small_to_split_is_handed_over_whole);
	RUN(free_returns_the_space_and_it_can_be_reused);
	RUN(freeing_null_is_a_no_op);
	RUN(adjacent_free_blocks_are_merged);
	RUN(fragmentation_without_merging_would_be_visible_here);
	RUN(double_free_is_refused_and_reported);
	RUN(freeing_a_pointer_the_heap_never_gave_out_is_refused);
	RUN(a_trampled_header_is_caught_by_the_magic_number);
	RUN(exhausting_the_heap_returns_null_rather_than_overrunning);
	RUN(many_allocations_and_frees_leave_the_heap_whole);
	RUN(stats_tolerate_null_arguments);

	fake_pmm_free_all();

	return test_report("kheap");
}
