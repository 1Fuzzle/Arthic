/* paging.c - taking over the page tables.
 *
 * boot.s built the smallest structure that would get us into 64-bit: one PML4,
 * one PDPT, and a PD of 512 huge pages covering the first gigabyte. That was
 * the right choice there - it is ten instructions and it cannot go wrong.
 *
 * It is the wrong structure to keep. A 2 MB huge page carries one set of
 * permissions for all 2 MB of it, so kernel code and kernel data inevitably
 * share a page and no useful protection is possible. Replacing them with 4 KB
 * pages costs 64 tables for 128 MB of RAM - about 256 KB - and buys per-page
 * permissions, which is the whole point of having an MMU.
 *
 * THE SWITCHOVER
 *
 * Unlike the 32-bit kernel, paging is ALREADY ON when this runs - it had to be,
 * or we would not be in long mode. So the new tables cannot be built by writing
 * to arbitrary physical addresses; they have to be built through the mapping
 * that already exists. That works because boot.s identity-mapped the first
 * gigabyte, and everything we allocate lives inside it.
 *
 * Then one write to CR3 swaps the entire structure. The instruction after it is
 * fetched through the new tables, so those tables had better map the code we
 * are standing on - identity mapping again, doing quiet work.
 */

#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "string.h"
#include "terminal.h"

#define ENTRIES 512

extern uint64_t kernel_text_start;
extern uint64_t kernel_rodata_end;

extern volatile uint64_t fault_resume_rip;   /* interrupts.s */
extern int probe_write(volatile uint64_t *addr, uint64_t value);

static uint64_t *pml4 = 0;
static uint64_t  mapped_limit = 0;

/* Split a virtual address into its four indices.
 *
 * The shifts are 39, 30, 21, 12 - nine bits each, because 512 entries needs
 * nine bits to index. The bottom twelve are the offset within a page. Add them
 * up: 9+9+9+9+12 = 48, which is exactly the 48-bit address space. The other 16
 * bits of a pointer are not addressing anything.
 */
static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index(uint64_t v)   { return (v >> 12) & 0x1FF; }

/* Follow one level down, creating the next table if asked. */
static uint64_t *next_level(uint64_t *table, uint64_t index, int create)
{
	if (!(table[index] & PAGE_PRESENT)) {
		if (!create)
			return 0;

		uint64_t frame = pmm_alloc_frame();
		if (!frame)
			return 0;

		kmemset((void *) frame, 0, PAGE_SIZE);

		/* Permissions are ANDed down the levels, so an upper entry must
		 * permit at least what the pages below it permit. Grant broadly here
		 * and be specific at the leaf.
		 *
		 * NX is the exception: it is ORed down. Setting it on an upper entry
		 * would make everything beneath non-executable regardless of what the
		 * leaves say. Never set it above the leaf. */
		table[index] = frame | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
	}

	return (uint64_t *)(table[index] & 0x000FFFFFFFFFF000ull);
}

int paging_map(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags)
{
	uint64_t *pdpt = next_level(pml4, pml4_index(virtual_addr), 1);
	if (!pdpt) return 0;

	uint64_t *pd = next_level(pdpt, pdpt_index(virtual_addr), 1);
	if (!pd) return 0;

	uint64_t *pt = next_level(pd, pd_index(virtual_addr), 1);
	if (!pt) return 0;

	pt[pt_index(virtual_addr)] = (physical_addr & 0x000FFFFFFFFFF000ull) | flags;

	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
	return 1;
}

static uint64_t *entry_for(uint64_t virtual_addr)
{
	uint64_t *pdpt = next_level(pml4, pml4_index(virtual_addr), 0);
	if (!pdpt) return 0;

	uint64_t *pd = next_level(pdpt, pdpt_index(virtual_addr), 0);
	if (!pd) return 0;

	uint64_t *pt = next_level(pd, pd_index(virtual_addr), 0);
	if (!pt) return 0;

	return &pt[pt_index(virtual_addr)];
}

void paging_set_flags(uint64_t virtual_addr, uint64_t flags)
{
	uint64_t *entry = entry_for(virtual_addr);
	if (!entry)
		return;

	*entry = (*entry & 0x000FFFFFFFFFF000ull) | flags;
	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

void paging_unmap(uint64_t virtual_addr)
{
	uint64_t *entry = entry_for(virtual_addr);
	if (!entry)
		return;

	*entry = 0;
	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

void paging_set_fault_resume(uint64_t rip)
{
	fault_resume_rip = rip;
}

int paging_probe_write(volatile uint64_t *addr, uint64_t value)
{
	return probe_write(addr, value);
}

static void page_fault_handler(struct registers *regs)
{
	if (fault_resume_rip) {
		regs->rip = fault_resume_rip;
		fault_resume_rip = 0;
		return;
	}

	uint64_t faulting_address;
	__asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));

	int present  = regs->err_code & 0x01;
	int write    = regs->err_code & 0x02;
	int user     = regs->err_code & 0x04;
	int reserved = regs->err_code & 0x08;
	int fetch    = regs->err_code & 0x10;

	kprintf("\n*** PAGE FAULT at 0x%lx\n", faulting_address);
	kprintf("    rip=0x%lx  cause: %s, %s, %s%s\n",
	        regs->rip,
	        present ? "protection violation" : "page not present",
	        fetch   ? "instruction fetch" : (write ? "write" : "read"),
	        user    ? "ring 3" : "ring 0",
	        reserved ? ", reserved bit set" : "");
	kprintf("    system halted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

void paging_init(void)
{
	/* Everything below is written through boot.s's identity mapping, which is
	 * still live. Frames come from the allocator, which only hands out memory
	 * inside the gigabyte that mapping covers. */
	uint64_t root = pmm_alloc_frame();
	if (!root) {
		kprintf("paging: no memory for a PML4\n");
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	kmemset((void *) root, 0, PAGE_SIZE);
	pml4 = (uint64_t *) root;

	uint64_t top = pmm_memory_top();

	/* Cap at the gigabyte boot.s mapped. Anything beyond it is unreachable
	 * while we are still building through the old tables - raising this means
	 * teaching boot.s to map more first, which is a deliberate change rather
	 * than something to do by accident. */
	if (top > 0x40000000ull)
		top = 0x40000000ull;

	for (uint64_t addr = 0; addr < top; addr += PAGE_SIZE) {
		if (!paging_map(addr, addr, PAGE_PRESENT | PAGE_WRITE)) {
			kprintf("paging: ran out of memory building tables\n");
			for (;;)
				__asm__ volatile ("cli; hlt");
		}
	}

	mapped_limit = top;

	/* Kernel code and constants: present, executable, not writable.
	 * Everything else stays writable and, thanks to the NX default below,
	 * non-executable - which is W^X, arriving free with the architecture
	 * rather than needing PAE bolted on. */
	uint64_t text_start = (uint64_t)&kernel_text_start & ~0xFFFull;
	uint64_t ro_end     = ((uint64_t)&kernel_rodata_end + PAGE_SIZE - 1) & ~0xFFFull;

	for (uint64_t addr = 0; addr < top; addr += PAGE_SIZE) {
		if (addr >= text_start && addr < ro_end)
			paging_set_flags(addr, PAGE_PRESENT);              /* r-x */
		else
			paging_set_flags(addr, PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
	}

	isr_install_handler(14, page_fault_handler);

	/* One write, and the entire address space changes meaning. */
	__asm__ volatile ("mov %0, %%cr3" : : "r"(pml4) : "memory");
}

uint64_t paging_mapped_limit(void)
{
	return mapped_limit;
}
