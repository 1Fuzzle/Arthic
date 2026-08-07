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
 * WHY WE MAP ALL OF RAM
 *
 * We identity-map every usable physical address — virtual X to physical X. A
 * mature kernel does not do this, but it makes everything above this layer
 * dramatically simpler: any frame the allocator hands out is immediately usable
 * without mapping it first. The heap depends on that.
 *
 * The page tables themselves come from the frame allocator, which raises the
 * obvious question of how we write to them before they are mapped. The answer
 * is timing: paging is still OFF while we build them, so every physical address
 * is directly writable. By the time it is on, everything is mapped. Ordering is
 * doing real work here.
 *
 * There is also a dangerous moment worth naming. The instruction after the one
 * that enables paging is fetched THROUGH the MMU. If its address were not
 * mapped, the CPU would fault fetching it, fault reaching the handler, and
 * triple-fault. Identity mapping makes the transition invisible.
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

#define TABLE_COVERAGE (ENTRIES * PAGE_SIZE)   /* one page table covers 4 MB */

/* The CPU requires these to be page-aligned — the low 12 bits of the address
 * are used for flags, so a misaligned table would have its address silently
 * mangled. `aligned(4096)` is the compiler doing that for us.
 *
 * The directory is static; the page tables come from the frame allocator. */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(4096)));
static uint32_t mapped_limit = 0;

/* From linker.ld — the boundaries of our own code. */
extern uint32_t kernel_text_start;
extern uint32_t kernel_rodata_end;

/* Find the page table entry for a virtual address, or 0 if unmapped.
 *
 * A virtual address splits into three parts:
 *   bits 31-22  which page directory entry  (which 4 MB region)
 *   bits 21-12  which page table entry      (which page within it)
 *   bits 11-0   offset within the page      (untouched by translation)
 */
static uint32_t *entry_for(uint32_t virtual_addr)
{
	uint32_t dir_index = virtual_addr >> 22;
	uint32_t tbl_index = (virtual_addr >> 12) & 0x3FF;

	if (!(page_directory[dir_index] & PAGE_PRESENT))
		return 0;

	uint32_t *table = (uint32_t *)(page_directory[dir_index] & 0xFFFFF000);
	return &table[tbl_index];
}

void paging_set_flags(uint32_t virtual_addr, uint32_t flags)
{
	uint32_t *entry = entry_for(virtual_addr);
	if (!entry)
		return;

	/* Keep the physical address, replace the flags. */
	*entry = (*entry & 0xFFFFF000) | flags;

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
/* Where to resume if the next fault is an expected one.
 *
 * Real kernels need this. When the kernel touches a pointer handed to it by
 * ring 3, that pointer may be garbage — and dying because a user program lied
 * is not acceptable. Linux calls the equivalent an exception table; it is how
 * copy_from_user survives a bad address.
 *
 * The mechanism: record a resume address, do the risky access, and if it
 * faults, the handler rewrites the saved EIP so `iret` returns to the resume
 * point instead of to the instruction that failed. We can do that because the
 * saved registers live on the stack we were handed, and editing them edits
 * what iret restores.
 */
extern volatile uint32_t fault_resume_eip;   /* defined in interrupts.s */

void paging_set_fault_resume(uint32_t eip)
{
	fault_resume_eip = eip;
}

static void page_fault_handler(struct registers *regs)
{
	if (fault_resume_eip) {
		regs->eip = fault_resume_eip;
		fault_resume_eip = 0;
		return;   /* iret goes to the resume point; the machine survives */
	}

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

	uint32_t top = pmm_memory_top();

	/* Round up, so a partial final 4 MB region still gets a table. */
	uint32_t tables_needed = (top + TABLE_COVERAGE - 1) / TABLE_COVERAGE;
	if (tables_needed > ENTRIES)
		tables_needed = ENTRIES;

	/* Identity-map all of it: virtual X to physical X.
	 *
	 * Supervisor-only from the outset (no PAGE_USER). Ring 3 will get its
	 * own mappings when user mode exists; it must never see kernel memory
	 * merely because the kernel mapped it first. */
	for (uint32_t t = 0; t < tables_needed; t++) {
		/* A page table is exactly one frame — 1024 entries of 4 bytes is
		 * 4096 bytes. Not a coincidence. */
		uint32_t table_phys = pmm_alloc_frame();
		if (!table_phys) {
			kprintf("paging: out of memory building page tables\n");
			for (;;)
				__asm__ volatile ("cli; hlt");
		}

		uint32_t *table = (uint32_t *) table_phys;

		for (uint32_t i = 0; i < ENTRIES; i++) {
			uint32_t physical = t * TABLE_COVERAGE + i * PAGE_SIZE;
			table[i] = physical | PAGE_PRESENT | PAGE_WRITE;
		}

		page_directory[t] = table_phys | PAGE_PRESENT | PAGE_WRITE;
	}

	mapped_limit = tables_needed * TABLE_COVERAGE;

	/* Now take write permission away from our own code and constants.
	 * Everything from the start of .text to the end of .rodata becomes
	 * read-only — present, but no PAGE_WRITE bit. */
	uint32_t text_start = (uint32_t) &kernel_text_start & ~(PAGE_SIZE - 1);
	uint32_t ro_end     = ((uint32_t) &kernel_rodata_end + PAGE_SIZE - 1)
	                      & ~(PAGE_SIZE - 1);

	for (uint32_t addr = text_start; addr < ro_end; addr += PAGE_SIZE) {
		uint32_t *entry = entry_for(addr);
		if (entry)
			*entry = (*entry & 0xFFFFF000) | PAGE_PRESENT;  /* not writable */
	}

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

uint32_t paging_mapped_limit(void)
{
	return mapped_limit;
}

/* Attempt a write, surviving a fault. The real work is in interrupts.s. */
extern int probe_write(volatile uint32_t *addr, uint32_t value);

int paging_probe_write(volatile uint32_t *addr, uint32_t value)
{
	return probe_write(addr, value);
}

/* Give ring 3 access to a range of pages.
 *
 * Note the deliberate asymmetry: `writable` is a parameter, because code should
 * be mapped read-only and data writable, and nothing should be both. That is
 * as close to W^X as 32-bit paging permits — no NX bit means user code pages
 * are executable whether we like it or not, but at least they are not writable.
 */
void paging_make_user(uint32_t start, uint32_t end, int writable)
{
	uint32_t flags = PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITE : 0);

	for (uint32_t addr = start & ~(PAGE_SIZE - 1); addr < end; addr += PAGE_SIZE) {
		/* PERMISSIONS ARE THE AND OF BOTH LEVELS.
		 *
		 * A translation walks the page directory and then the page table, and
		 * an access is allowed only if BOTH entries permit it. Setting the
		 * user bit on the page table entry alone achieves nothing while the
		 * directory entry above it is supervisor-only — the CPU stops at the
		 * first level and refuses.
		 *
		 * This is easy to miss because the failure looks identical to
		 * forgetting the page table entry, and everything you inspect at the
		 * table level looks correct.
		 *
		 * Opening the directory entry is not itself a hole: every page under
		 * it still needs its own user bit, and they do not have one. The
		 * directory entry grants permission to ask, not permission to read. */
		page_directory[addr >> 22] |= PAGE_USER;

		paging_set_flags(addr, flags);
	}
}
