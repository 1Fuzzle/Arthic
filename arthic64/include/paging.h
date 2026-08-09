/* paging.h - four-level paging.
 *
 * boot.s built a minimal set of tables with 2 MB huge pages, just enough to
 * reach 64-bit code. This replaces them with a managed structure using 4 KB
 * pages, so individual pages can carry their own permissions.
 *
 * FOUR LEVELS
 *
 *   PML4   512 entries, each covering 512 GB
 *   PDPT   512 entries, each covering 1 GB
 *   PD     512 entries, each covering 2 MB
 *   PT     512 entries, each covering one 4 KB page
 *
 * 512^4 * 4 KB is 256 TB, which is the address space 64-bit x86 actually
 * provides - not the full 2^64. The top 16 bits of a pointer must be copies of
 * bit 47, a rule called canonical form. Addresses that break it fault, which is
 * why a 64-bit kernel sees "non-canonical" faults where a 32-bit one would see
 * a wild pointer land somewhere plausible.
 */
#ifndef ARTHIC_PAGING_H
#define ARTHIC_PAGING_H

#include <stdint.h>

#define PAGE_PRESENT  0x001ull
#define PAGE_WRITE    0x002ull
#define PAGE_USER     0x004ull
#define PAGE_HUGE     0x080ull
#define PAGE_NX       (1ull << 63)   /* enabled by EFER.NXE, set in boot.s */

void     paging_init(void);
uint64_t paging_mapped_limit(void);

int      paging_map(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void     paging_unmap(uint64_t virtual_addr);

/* ---- address spaces -------------------------------------------------------- */
uint64_t paging_kernel_directory(void);
uint64_t paging_create_address_space(void);
void     paging_destroy_address_space(uint64_t pml4_phys);
void     paging_switch(uint64_t pml4_phys);

/* Mark an already-mapped range accessible from ring 3. Data gets NX; code does
 * not, since it must remain executable - the same asymmetry as the 32-bit
 * branch's W^X, just no longer conditional on hardware support. */
void     paging_make_user(uint64_t start, uint64_t end, int writable);
void     paging_set_flags(uint64_t virtual_addr, uint64_t flags);

void     paging_set_fault_resume(uint64_t rip);
int      paging_probe_write(volatile uint64_t *addr, uint64_t value);

#endif
