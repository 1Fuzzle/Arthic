/* paging.h — virtual memory.
 *
 * Turns on the MMU and gives the kernel control over what each address means
 * and who may touch it.
 */
#ifndef ARTHIC_PAGING_H
#define ARTHIC_PAGING_H

#include <stdint.h>

/* Flags in a page directory or page table entry. The top 20 bits hold a
 * physical address (always page-aligned, so the low 12 bits are free for
 * flags — that is not a coincidence, it is why pages are 4 KB).
 *
 * Note what is NOT here: a no-execute bit. Plain 32-bit paging has none. Any
 * readable page is executable, full stop. NX requires PAE or long mode. See
 * the comment in paging.c.
 */
#define PAGE_PRESENT  0x001   /* 0 here means the page is not mapped at all */
#define PAGE_WRITE    0x002   /* clear = read-only                          */
#define PAGE_USER     0x004   /* clear = ring 0 only                        */
#define PAGE_ACCESSED 0x020   /* CPU sets this when the page is read        */
#define PAGE_DIRTY    0x040   /* CPU sets this when the page is written     */

void paging_init(void);

/* Change the flags on one already-mapped page. Used to make the kernel's own
 * code read-only after the initial mapping is built. */
void paging_set_flags(uint32_t virtual_addr, uint32_t flags);

/* How much memory is identity-mapped. */
uint32_t paging_mapped_limit(void);

/* Arm recovery: if the next page fault happens, resume at `eip` instead of
 * halting. Pass 0 to disarm. Used for deliberately risky accesses. */
void paging_set_fault_resume(uint32_t eip);

/* Try to write to an address. Returns 1 if it worked, 0 if it page-faulted.
 * The machine survives either way. */
int paging_probe_write(volatile uint32_t *addr, uint32_t value);

/* Mark a range as accessible from ring 3. */
void paging_make_user(uint32_t start, uint32_t end, int writable);

#endif
