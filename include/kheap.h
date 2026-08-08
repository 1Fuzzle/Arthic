/* kheap.h — the kernel heap.
 *
 * The frame allocator hands out 4096 bytes at a time. That is the wrong
 * granularity for most things: a linked list node might want 16 bytes, a
 * string 40. This sits on top and hands out arbitrary sizes.
 *
 * `k` prefix throughout, as with kprintf and kmemset — these are the kernel's
 * versions, not libc's, and the naming should never let you forget it.
 */
#ifndef ARTHIC_KHEAP_H
#define ARTHIC_KHEAP_H

#include <stdint.h>
#include <stddef.h>

/* Reserve the heap's memory. Returns 0 if it could not be reserved, which is
 * fatal in practice - almost everything above this layer allocates. */
int   kheap_init(void);

/* Returns a pointer to at least `size` usable bytes, or NULL if the heap is
 * exhausted. Callers MUST check for NULL — there is no exception mechanism
 * here, and a NULL dereference in kernel space is a page fault and a halt. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

void kheap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks);

#endif
