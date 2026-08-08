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

/* Could ring 3 itself touch every byte of [addr, addr + length) in the address
 * space that is currently loaded? Returns 1 if it could, 0 otherwise.
 *
 * This is the question a syscall needs to ask about a pointer it was handed.
 * Asking the page tables rather than comparing against a remembered address
 * range means the answer cannot disagree with what the hardware would do, and
 * it is per address space for free - the tables being consulted are the ones
 * the CPU is using for this task.
 *
 * `need_write` distinguishes a buffer the kernel reads from one it writes into.
 * A pointer into the program's own code passes the first and must fail the
 * second, or the kernel becomes the way a program edits its read-only pages.
 */
int paging_user_access_ok(uint32_t addr, uint32_t length, int need_write);

/* Map one page of virtual memory onto a physical frame, creating the page
 * table if that region has none yet. Returns 0 if it could not allocate a
 * table. */
int paging_map(uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);

/* ---- address spaces -------------------------------------------------------
 * Up to now there has been one page directory and every address meant the same
 * thing to everyone. A separate address space per program is what lets two
 * programs both be loaded at 0x20000000 and not be the same memory.
 */

/* Physical address of the kernel's own directory. */
uint32_t paging_kernel_directory(void);

/* A fresh directory sharing the kernel's mappings and nothing else. Returns 0
 * on failure. */
uint32_t paging_create_address_space(void);

/* Free a directory and any page tables it added beyond the kernel's. Does not
 * touch the frames those tables mapped - the caller owns those. */
void paging_destroy_address_space(uint32_t page_dir_phys);

/* Make `page_dir_phys` the active address space. Passing 0 means the kernel's.
 * Every subsequent map and unmap applies to it. */
void paging_switch(uint32_t page_dir_phys);

/* Remove a mapping. The physical frame is not freed - that is the caller's
 * business, and conflating the two is how you get double frees. */
void paging_unmap(uint32_t virtual_addr);

#endif
