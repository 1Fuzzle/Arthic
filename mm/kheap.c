/* kheap.c — the kernel heap.
 *
 * THE SHAPE OF THE PROBLEM
 *
 * The frame allocator deals in 4096-byte units. Almost nothing wants exactly
 * 4096 bytes. So this layer takes a large contiguous region and carves it into
 * variable-sized pieces on request, then stitches the pieces back together as
 * they are returned.
 *
 * HOW IT WORKS
 *
 * Every block, free or in use, carries a header immediately before its payload.
 * The headers form a doubly-linked list in address order, covering the whole
 * heap with no gaps. A block is either free or it is not; there is no separate
 * free list.
 *
 *   [header][......payload......][header][...payload...][header][...]
 *
 * Allocation is FIRST FIT: walk the list, take the first free block big enough,
 * and if it is much bigger than needed, split it in two. Freeing marks the block
 * free and merges it with any free neighbours.
 *
 * WHY MERGING MATTERS
 *
 * Without it, allocate-free-allocate-free eventually leaves the heap as a mosaic
 * of small free blocks that cannot satisfy any large request, even though most
 * of the heap is technically free. That is external fragmentation. Merging
 * adjacent free blocks is the cheapest defence, and it is why the list is
 * doubly-linked — you cannot merge with the block before you unless you can find
 * it.
 *
 * First fit is not the best strategy. Best fit wastes less; buddy allocators are
 * far faster; slab allocators beat all of them for fixed-size objects. Every one
 * of those is a refinement of this, and none of them is worth writing until this
 * one is measurably a problem.
 *
 * THE MAGIC NUMBER
 *
 * Each header holds a known constant. kfree checks it before touching anything,
 * which catches freeing a pointer that never came from kmalloc, freeing the same
 * pointer twice, and a buffer overflow in the block before this one having
 * trampled the header. None of those are exotic — they are the three most common
 * heap bugs in C, and in a kernel each of them is a potential exploit rather than
 * a crash. Checking costs one comparison.
 */

#include "kheap.h"
#include "pmm.h"
#include "string.h"
#include "terminal.h"

#define HEAP_FRAMES 256                        /* 256 * 4 KB = 1 MB */
#define HEAP_SIZE   (HEAP_FRAMES * PAGE_SIZE)

#define BLOCK_MAGIC 0xA47C0DE5u

/* Payloads are 8-byte aligned. Misaligned accesses are merely slow on x86, but
 * they fault outright on other architectures, and returning aligned memory is
 * what every caller assumes without asking. */
#define ALIGNMENT 8
#define ALIGN_UP(n) (((n) + (ALIGNMENT - 1)) & ~((size_t)(ALIGNMENT - 1)))

struct block {
	uint32_t      magic;
	size_t        size;      /* usable payload bytes, not counting the header */
	int           free;
	struct block *next;
	struct block *prev;
};

#define HEADER_SIZE ALIGN_UP(sizeof(struct block))

static struct block *heap_start = 0;
static uint32_t      heap_bytes = 0;

/* payload address = header address + header size, and back again. Wrapping
 * this in functions rather than scattering the arithmetic means the pointer
 * maths happens in exactly two places. */
static void *payload_of(struct block *b)
{
	return (void *)((uint8_t *) b + HEADER_SIZE);
}

static struct block *header_of(void *payload)
{
	return (struct block *)((uint8_t *) payload - HEADER_SIZE);
}

int kheap_init(void)
{
	/* One contiguous run. It has to be contiguous because the heap hands out
	 * regions that may span page boundaries, and a caller that gets 8 KB
	 * expects 8 KB of consecutive addresses. */
	uint32_t base = pmm_alloc_frames(HEAP_FRAMES);

	/* Report the failure rather than leaving heap_start at 0 and letting every
	 * later kmalloc return NULL for reasons the caller cannot see. */
	if (!base)
		return 0;

	heap_start = (struct block *) base;
	heap_bytes = HEAP_SIZE;

	/* One enormous free block covering everything. Every later block is a
	 * descendant of this one by splitting. */
	heap_start->magic = BLOCK_MAGIC;
	heap_start->size  = HEAP_SIZE - HEADER_SIZE;
	heap_start->free  = 1;
	heap_start->next  = 0;
	heap_start->prev  = 0;

	return 1;
}

/* Split `b` so it holds exactly `size` bytes, with the remainder becoming a new
 * free block after it.
 *
 * Only worth doing if the leftover can hold a header plus something useful —
 * otherwise you create a block too small to ever be allocated, which is pure
 * waste plus a list node to walk past forever. */
static void split(struct block *b, size_t size)
{
	if (b->size < size + HEADER_SIZE + ALIGNMENT)
		return;

	struct block *rest = (struct block *)((uint8_t *) payload_of(b) + size);

	rest->magic = BLOCK_MAGIC;
	rest->size  = b->size - size - HEADER_SIZE;
	rest->free  = 1;
	rest->next  = b->next;
	rest->prev  = b;

	if (b->next)
		b->next->prev = rest;

	b->next = rest;
	b->size = size;
}

void *kmalloc(size_t size)
{
	if (!heap_start || size == 0)
		return 0;

	size = ALIGN_UP(size);

	for (struct block *b = heap_start; b; b = b->next) {
		if (b->free && b->size >= size) {
			split(b, size);
			b->free = 0;
			return payload_of(b);
		}
	}

	return 0;   /* nothing big enough — caller must handle this */
}

/* Merge `b` with the block after it, if both are free. */
static void merge_with_next(struct block *b)
{
	struct block *n = b->next;

	if (!n || !n->free || !b->free)
		return;

	b->size += HEADER_SIZE + n->size;
	b->next = n->next;

	if (n->next)
		n->next->prev = b;

	/* Wipe the absorbed header. Not required for correctness, but it means a
	 * stale pointer to it fails the magic check instead of finding a
	 * plausible-looking block. */
	n->magic = 0;
}

void kfree(void *ptr)
{
	if (!ptr)
		return;   /* freeing NULL is defined as a no-op, same as real free */

	struct block *b = header_of(ptr);

	/* The guard. Any of these means something is already wrong, and
	 * continuing would corrupt the heap in a way that surfaces later
	 * somewhere unrelated — the worst kind of bug to chase. */
	if (b->magic != BLOCK_MAGIC) {
		kprintf("kfree: bad or corrupted block at 0x%x - ignoring\n",
		        (uint32_t) ptr);
		return;
	}

	if (b->free) {
		kprintf("kfree: double free at 0x%x - ignoring\n", (uint32_t) ptr);
		return;
	}

	b->free = 1;

	/* Merge forwards, then backwards. Doing both means a block freed between
	 * two free neighbours becomes one large block rather than three small
	 * ones. */
	merge_with_next(b);
	if (b->prev && b->prev->free)
		merge_with_next(b->prev);
}

void kheap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks)
{
	uint32_t u = 0, n = 0;

	for (struct block *b = heap_start; b; b = b->next) {
		n++;
		if (!b->free)
			u += HEADER_SIZE + b->size;
	}

	if (total)  *total  = heap_bytes;
	if (used)   *used   = u;
	if (blocks) *blocks = n;
}
