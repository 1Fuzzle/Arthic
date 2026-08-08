/* paging.h - virtual memory, with PAE.
 *
 * PAE (Physical Address Extension) is a 32-bit paging mode with 64-bit page
 * table entries. The extra width is what carries the NX bit - bit 63 - which
 * plain 32-bit paging has no room for.
 *
 * That bit is the whole reason for the change. Without it, every page a
 * program can read, it can also execute: a buffer full of attacker-controlled
 * data is executable code waiting to be jumped to. With it, data pages can be
 * marked non-executable and the CPU refuses.
 *
 * THREE LEVELS INSTEAD OF TWO
 *
 * Entries are twice as wide, so half as many fit in a 4 KB table - 512 instead
 * of 1024. That costs a level:
 *
 *   PDPT     4 entries, each covering 1 GB
 *   PD     512 entries, each covering 2 MB
 *   PT     512 entries, each covering one 4 KB page
 *
 * CR3 points at the PDPT, which is only 32 bytes and must be 32-byte aligned.
 */
#ifndef ARTHIC_PAGING_H
#define ARTHIC_PAGING_H

#include <stdint.h>

/* Flags in a page table entry. The top bits of the physical address occupy the
 * middle; the low 12 and the very top are ours. */
#define PAGE_PRESENT  0x001ull   /* 0 here means the page is not mapped at all */
#define PAGE_WRITE    0x002ull   /* clear = read-only                          */
#define PAGE_USER     0x004ull   /* clear = ring 0 only                        */
#define PAGE_ACCESSED 0x020ull
#define PAGE_DIRTY    0x040ull

/* Bit 63. Only meaningful once EFER.NXE is set - before that the CPU treats it
 * as reserved and faults on any entry that has it. Enabling the bit and
 * enabling the feature are two separate steps, and doing them out of order is
 * an instant triple fault. */
#define PAGE_NX       (1ull << 63)

void paging_init(void);

/* Change the flags on one already-mapped page. */
void paging_set_flags(uint32_t virtual_addr, uint64_t flags);

/* How much memory is mapped. */
uint32_t paging_mapped_limit(void);

/* Did the CPU actually give us NX? Checked at boot; false on very old
 * hardware, in which case data pages stay executable and we say so rather than
 * pretending otherwise. */
int paging_nx_available(void);

/* Arm recovery: if the next page fault happens, resume at `eip` instead of
 * halting. Pass 0 to disarm. */
void paging_set_fault_resume(uint32_t eip);

/* Try to write to an address. Returns 1 if it worked, 0 if it page-faulted. */
int paging_probe_write(volatile uint32_t *addr, uint32_t value);

/* Mark a range as accessible from ring 3. Data is mapped non-executable when
 * the hardware allows it. */
void paging_make_user(uint32_t start, uint32_t end, int writable);

/* ---- address spaces ------------------------------------------------------- */

uint32_t paging_kernel_directory(void);
uint32_t paging_create_address_space(void);
void     paging_destroy_address_space(uint32_t pdpt_phys);
void     paging_switch(uint32_t pdpt_phys);

/* Map one page, creating tables as needed. */
int  paging_map(uint32_t virtual_addr, uint32_t physical_addr, uint64_t flags);
void paging_unmap(uint32_t virtual_addr);

#endif
