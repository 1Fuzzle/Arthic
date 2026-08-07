/* paging.c — turning on the MMU.
 *
 * WHAT CHANGES
 *
 * Until this file runs, every address the CPU uses is a physical address: write
 * to 0xB8000 and you touch the actual chip at 0xB8000. After it runs, every
 * address goes through a translation table first. 0xB8000 means whatever we say
 * it means.
 *
 * That indirection is the foundation of nearly everything an operating system
 * does. Two programs can both believe they own address 0x400000 and be given
 * different physical memory. A program can be given an address space with holes
 * in it, so a stray pointer hits nothing and faults instead of silently
 * corrupting something else. And — the part you care about — each page carries
 * permissions the hardware enforces on every single access, with no cost,
 * because the check happens in the MMU rather than in software.
 *
 * THE STRUCTURE
 *
 * Two levels. A page directory of 1024 entries, each pointing at a page table
 * of 1024 entries, each describing one 4 KB page. 1024 * 1024 * 4096 = 4 GB,
 * which is the whole 32-bit address space.
 *
 * An entry is one 32-bit word: the top 20 bits are a physical address and the
 * low 12 are flags. That works because pages are 4 KB aligned, so the low 12
 * bits of any page address are always zero and would otherwise be wasted.
 *
 * THE DANGEROUS MOMENT
 *
 * The instruction after the one that enables paging is fetched through the MMU.
 * If the address it lives at is not mapped, the CPU faults trying to fetch it,
 * faults trying to reach the fault handler, and triple-faults. So before
 * switching on, we identity-map the kernel — virtual address X maps to physical
 * address X — which makes the transition invisible to everything already
 * running.
 *
 * ON W^X, HONESTLY
 *
 * You asked for Arthic to be secure, so here is the awkward truth about this
 * step: plain 32-bit paging has no no-execute bit. There is a read/write bit
 * and a user/supervisor bit, and that is all. Any page the CPU can read, it can
 * execute. Real W^X — data that cannot be executed — needs PAE or 64-bit long
 * mode, both of which have a wider entry format with an NX bit in it.
 *
 * So what we CAN enforce here is half of it, and it is still worth having:
 *
 *   - kernel code and read-only data are mapped read-only, so a bug cannot
 *     rewrite the kernel's own instructions
 *   - CR0.WP is set, which is what makes that apply to ring 0 as well; without
 *     it the kernel is exempt from its own read-only bits and the protection
 *     is decorative
 *   - every kernel page is supervisor-only, so ring 3 cannot read kernel memory
 *     once user mode exists
 *
 * The missing NX is a real argument for moving to long mode sooner rather than
 * later, and worth remembering when that decision comes up.
 */

#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "string.h"
#include "terminal.h"

#define ENTRIES 1024

/* How much to identity-map at boot. 8 MB comfortably covers the kernel, the
 * PMM bitmap and all the low-memory hardware regions including VGA. */
#define IDENTITY_TABLES 2
#define IDENTITY_LIMIT  (IDENTITY_TABLES * ENTRIES * PAGE_SIZE)

/* The CPU requires these to be page-aligned — the low 12 bits of the address
 * are used for flags, so a misaligned table would have its address silently
 * mangled. `aligned(4096)` is the compiler doing that for us.
 *
 * These are static arrays rather than frames from the PMM because the initial
 * mapping has to exist before we can safely allocate anything. Page tables
 * created later will come from pmm_alloc_frame. */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(4096)));
static uint32_t page_tables[IDENTITY_TABLES][ENTRIES] __attribute__((aligned(4096)));

/* From linker.ld — the boundaries of our own code. */
extern uint32_t kernel_text_start;
extern uint32_t kernel_text_end;
extern uint32_t kernel_rodata_end;

static uint32_t *table_for(uint32_t virtual_addr)
{
	uint32_t dir_index = virtual_addr >> 22;          /* top 10 bits    */
	if (dir_index >= IDENTITY_TABLES)
		return 0;
	return page_tables[dir_index];
}

void paging_set_flags(uint32_t virtual_addr, uint32_t flags)
{
	uint32_t *table = table_for(virtual_addr);
	if (!table)
		return;

	uint32_t table_index = (virtual_addr >> 12) & 0x3FF;   /* middle 10 bits */

	/* Keep the physical address, replace the flags. */
	table[table_index] = (table[table_index] & 0xFFFFF000) | flags;

	/* The CPU caches translations in the TLB. Change an entry without
	 * telling it and it will keep using the stale one — a bug that looks
	 * like the hardware ignoring you. `invlpg` drops one page's cached
	 * translation. */
	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

/* Page fault handler.
 *
 * Every page fault carries two pieces of information. CR2 holds the address
 * that was being accessed — the CPU puts it there and nowhere else, which is
 * why this needs inline assembly to read. The error code says what kind of
 * access it was.
 *
 * In a mature kernel most page faults are routine: the page exists on disk, or
 * needs allocating on first touch. Ours is a diagnostic, because nothing yet
 * produces a legitimate fault. But reporting properly rather than halting
 * silently is exactly the difference between a bug you can find and one you
 * cannot.
 */
static void page_fault_handler(struct registers *regs)
{
	uint32_t faulting_address;
	__asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));

	int present  =  regs->err_code & 0x1;   /* 0 = page not mapped at all */
	int write    =  regs->err_code & 0x2;   /* was it a write?            */
	int user     =  regs->err_code & 0x4;   /* did ring 3 do it?          */
	int reserved =  regs->err_code & 0x8;   /* malformed table entry      */

	kprintf("\n*** PAGE FAULT at 0x%x\n", faulting_address);
	kprintf("    eip=0x%x  cause: %s, %s, %s%s\n",
	        regs->eip,
	        present  ? "protection violation" : "page not present",
	        write    ? "write" : "read",
	        user     ? "ring 3" : "ring 0",
	        reserved ? ", reserved bit set" : "");
	kprintf("    system halted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

void paging_init(void)
{
	kmemset(page_directory, 0, sizeof(page_directory));

	/* Identity-map the low memory: virtual X to physical X.
	 *
	 * Supervisor-only from the outset (no PAGE_USER). Ring 3 will get its
	 * own mappings when user mode exists; it should never be able to see
	 * kernel memory just because the kernel happened to map it first. */
	for (uint32_t t = 0; t < IDENTITY_TABLES; t++) {
		for (uint32_t i = 0; i < ENTRIES; i++) {
			uint32_t physical = (t * ENTRIES + i) * PAGE_SIZE;
			page_tables[t][i] = physical | PAGE_PRESENT | PAGE_WRITE;
		}
		page_directory[t] = ((uint32_t) page_tables[t])
		                    | PAGE_PRESENT | PAGE_WRITE;
	}

	/* Now take write permission away from our own code and constants.
	 * Everything from the start of .text to the end of .rodata becomes
	 * read-only — present, but no PAGE_WRITE bit. */
	uint32_t text_start = (uint32_t) &kernel_text_start & ~(PAGE_SIZE - 1);
	uint32_t ro_end     = ((uint32_t) &kernel_rodata_end + PAGE_SIZE - 1)
	                      & ~(PAGE_SIZE - 1);

	for (uint32_t addr = text_start; addr < ro_end; addr += PAGE_SIZE)
		page_tables[addr >> 22][(addr >> 12) & 0x3FF] =
			addr | PAGE_PRESENT;    /* present, NOT writable */

	isr_install_handler(14, page_fault_handler);

	/* Point CR3 at the directory. This register is where the MMU looks. */
	__asm__ volatile ("mov %0, %%cr3" : : "r"(page_directory));

	/* Two bits in CR0, and the order they go in matters:
	 *
	 *   bit 16 (WP)  makes ring 0 obey read-only page bits. Without it the
	 *                kernel can write to pages it marked read-only and the
	 *                protection above achieves nothing at all.
	 *   bit 31 (PG)  enables paging.
	 *
	 * Setting WP first means protection is live the instant paging is. */
	uint32_t cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x00010000;                                  /* WP */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;                                  /* PG */
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

	/* Anything past here is running through the MMU. */
}

uint32_t paging_identity_limit(void)
{
	return IDENTITY_LIMIT;
}
