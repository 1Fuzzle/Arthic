/* paging.c - the MMU, in PAE mode.
 *
 * WHAT PAE BUYS AND WHAT IT COSTS
 *
 * Page table entries become 64 bits wide instead of 32. The extra room carries
 * the NX bit, and that is the entire reason we are here: without it, any page
 * a program can read it can also execute, so a buffer full of data the attacker
 * chose is executable code waiting to be jumped to. That is the shape of a
 * great many real exploits, and NX is what closes it.
 *
 * The cost is a third level of tables. Wider entries mean 512 fit in a page
 * instead of 1024, so the address split becomes:
 *
 *   bits 31-30   which PDPT entry   (which 1 GB)
 *   bits 29-21   which PD entry     (which 2 MB)
 *   bits 20-12   which PT entry     (which 4 KB page)
 *   bits 11-0    offset in the page
 *
 * CR3 points at the PDPT, which holds only 4 entries and must be 32-byte
 * aligned. Everything else is a 4 KB table like before.
 *
 * ORDER OF OPERATIONS
 *
 * Three things have to happen in a specific sequence, and getting it wrong is
 * a triple fault rather than an error message:
 *
 *   1. Set EFER.NXE, or the CPU treats bit 63 as reserved and faults on any
 *      entry that uses it.
 *   2. Set CR4.PAE, which changes how the CPU reads page tables.
 *   3. Load CR3 and set CR0.PG.
 *
 * WHY NOT LONG MODE
 *
 * 64-bit would also provide NX, and much else - but it rewrites boot.s, the
 * GDT, the IDT's gate format, the TSS, the context switch, and every piece of
 * inline assembly in the kernel. PAE reaches the same security property by
 * changing one file. The other benefits of 64-bit are real; they are simply
 * not what NX required.
 */

#include "paging.h"
#include "pmm.h"
#include "idt.h"
#include "string.h"
#include "terminal.h"
#include "usermode.h"
#include "task.h"

#define PDPT_ENTRIES 4
#define TABLE_ENTRIES 512
#define PD_COVERAGE  (2u * 1024 * 1024)      /* one PD entry covers 2 MB */

/* The PDPT must be 32-byte aligned; the rest are page-aligned. */
static uint64_t kernel_pdpt[PDPT_ENTRIES] __attribute__((aligned(32)));
static uint64_t kernel_pd[TABLE_ENTRIES]  __attribute__((aligned(4096)));

static uint64_t *active_pdpt = kernel_pdpt;
static uint32_t  mapped_limit = 0;
static uint32_t  kernel_pd_entries = 0;
static int       nx_supported = 0;

extern uint32_t kernel_text_start;
extern uint32_t kernel_rodata_end;

extern volatile uint32_t fault_resume_eip;   /* defined in interrupts.s */
extern int probe_write(volatile uint32_t *addr, uint32_t value);

/* ---- CPU feature and mode bits -------------------------------------------- */

static int cpu_has_nx(void)
{
	uint32_t eax, ebx, ecx, edx;

	/* Is there an extended CPUID leaf at all? Very old CPUs have none, and
	 * asking for 0x80000001 on one of those returns whatever leaf 0 returned,
	 * which would be nonsense read as a feature list. */
	__asm__ volatile ("cpuid"
	                  : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
	                  : "a"(0x80000000u));

	if (eax < 0x80000001u)
		return 0;

	__asm__ volatile ("cpuid"
	                  : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
	                  : "a"(0x80000001u));

	return (edx & (1u << 20)) != 0;      /* bit 20 = NX */
}

static void enable_nx(void)
{
	/* EFER is a model-specific register, reached through rdmsr/wrmsr rather
	 * than being an ordinary register. Bit 11 is NXE. */
	uint32_t low, high;

	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080u));
	low |= (1u << 11);
	__asm__ volatile ("wrmsr" : : "a"(low), "d"(high), "c"(0xC0000080u));
}

/* ---- walking the tables --------------------------------------------------- */

static uint64_t *pd_for(uint64_t *pdpt, uint32_t virtual_addr)
{
	uint32_t index = virtual_addr >> 30;

	if (!(pdpt[index] & PAGE_PRESENT))
		return 0;

	return (uint64_t *)(uint32_t)(pdpt[index] & 0xFFFFF000ull);
}

static uint64_t *entry_for(uint32_t virtual_addr)
{
	uint64_t *pd = pd_for(active_pdpt, virtual_addr);
	if (!pd)
		return 0;

	uint32_t pd_index = (virtual_addr >> 21) & 0x1FF;

	if (!(pd[pd_index] & PAGE_PRESENT))
		return 0;

	uint64_t *pt = (uint64_t *)(uint32_t)(pd[pd_index] & 0xFFFFF000ull);

	return &pt[(virtual_addr >> 12) & 0x1FF];
}

void paging_set_flags(uint32_t virtual_addr, uint64_t flags)
{
	uint64_t *entry = entry_for(virtual_addr);
	if (!entry)
		return;

	*entry = (*entry & 0xFFFFF000ull) | flags;

	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

int paging_map(uint32_t virtual_addr, uint32_t physical_addr, uint64_t flags)
{
	uint32_t pdpt_index = virtual_addr >> 30;
	uint32_t pd_index   = (virtual_addr >> 21) & 0x1FF;
	uint32_t pt_index   = (virtual_addr >> 12) & 0x1FF;

	if (!(active_pdpt[pdpt_index] & PAGE_PRESENT)) {
		uint32_t phys = pmm_alloc_frame();
		if (!phys)
			return 0;

		kmemset((void *) phys, 0, PAGE_SIZE);

		/* A PDPT entry carries only present, and the address. It has no
		 * user or write bits - unusually, permission checking starts at the
		 * PD level in PAE. */
		active_pdpt[pdpt_index] = (uint64_t) phys | PAGE_PRESENT;
	}

	uint64_t *pd = (uint64_t *)(uint32_t)(active_pdpt[pdpt_index] & 0xFFFFF000ull);

	if (!(pd[pd_index] & PAGE_PRESENT)) {
		uint32_t phys = pmm_alloc_frame();
		if (!phys)
			return 0;

		kmemset((void *) phys, 0, PAGE_SIZE);

		/* Permissions are ANDed down the levels, so the PD entry must allow
		 * at least what the pages under it allow. Grant broadly here and be
		 * specific per page. NX is the exception and works the other way: it
		 * is ORed down, so setting it here would make everything below
		 * non-executable regardless. Leave it clear at this level. */
		pd[pd_index] = (uint64_t) phys | PAGE_PRESENT | PAGE_WRITE
		               | (flags & PAGE_USER);
	} else if (flags & PAGE_USER) {
		pd[pd_index] |= PAGE_USER;
	}

	uint64_t *pt = (uint64_t *)(uint32_t)(pd[pd_index] & 0xFFFFF000ull);

	/* Drop NX silently if the CPU lacks it, rather than setting a reserved
	 * bit and faulting. paging_nx_available lets callers know. */
	if (!nx_supported)
		flags &= ~PAGE_NX;

	pt[pt_index] = (uint64_t) physical_addr | flags;

	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
	return 1;
}

void paging_unmap(uint32_t virtual_addr)
{
	uint64_t *entry = entry_for(virtual_addr);
	if (!entry)
		return;

	*entry = 0;
	__asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

/* ---- faults --------------------------------------------------------------- */

void paging_set_fault_resume(uint32_t eip)
{
	fault_resume_eip = eip;
}

int paging_probe_write(volatile uint32_t *addr, uint32_t value)
{
	return probe_write(addr, value);
}

static void page_fault_handler(struct registers *regs)
{
	if (fault_resume_eip) {
		regs->eip = fault_resume_eip;
		fault_resume_eip = 0;
		return;
	}

	uint32_t faulting_address;
	__asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_address));

	int present  = regs->err_code & 0x01;
	int write    = regs->err_code & 0x02;
	int user     = regs->err_code & 0x04;
	int reserved = regs->err_code & 0x08;

	/* Bit 4 means the fault was an INSTRUCTION FETCH rather than a data
	 * access. It only ever appears when NX is enabled, and it is how you tell
	 * "tried to execute this" from "tried to read it". */
	int fetch    = regs->err_code & 0x10;

	if (user) {
		kprintf("\n*** program terminated: %s at 0x%x (eip 0x%x)\n",
		        fetch   ? "tried to execute non-executable memory"
		        : present ? (write ? "wrote to read-only memory"
		                           : "protection violation")
		                  : "touched unmapped memory",
		        faulting_address, regs->eip);

		struct task *t = task_current();

		if (t && t->on_exit)
			task_terminate();

		usermode_return();
	}

	kprintf("\n*** PAGE FAULT at 0x%x\n", faulting_address);
	kprintf("    eip=0x%x  cause: %s, %s, %s%s%s\n",
	        regs->eip,
	        present  ? "protection violation" : "page not present",
	        fetch    ? "instruction fetch" : (write ? "write" : "read"),
	        user     ? "ring 3" : "ring 0",
	        reserved ? ", reserved bit set" : "",
	        "");
	kprintf("    system halted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

/* ---- setup ---------------------------------------------------------------- */

void paging_init(void)
{
	nx_supported = cpu_has_nx();

	if (nx_supported)
		enable_nx();          /* before any entry uses bit 63 */

	kmemset(kernel_pdpt, 0, sizeof(kernel_pdpt));
	kmemset(kernel_pd, 0, sizeof(kernel_pd));

	active_pdpt = kernel_pdpt;

	uint32_t top = pmm_memory_top();
	uint32_t pd_needed = (top + PD_COVERAGE - 1) / PD_COVERAGE;

	if (pd_needed > TABLE_ENTRIES)
		pd_needed = TABLE_ENTRIES;

	/* Identity-map all of RAM. Page tables come from the frame allocator,
	 * which is safe because paging is still off - every physical address is
	 * directly writable until the moment we switch it on. */
	for (uint32_t i = 0; i < pd_needed; i++) {
		uint32_t pt_phys = pmm_alloc_frame();
		if (!pt_phys) {
			kprintf("paging: out of memory building page tables\n");
			for (;;)
				__asm__ volatile ("cli; hlt");
		}

		uint64_t *pt = (uint64_t *) pt_phys;

		for (uint32_t j = 0; j < TABLE_ENTRIES; j++) {
			uint32_t physical = i * PD_COVERAGE + j * PAGE_SIZE;
			pt[j] = (uint64_t) physical | PAGE_PRESENT | PAGE_WRITE;
		}

		kernel_pd[i] = (uint64_t) pt_phys | PAGE_PRESENT | PAGE_WRITE;
	}

	kernel_pdpt[0] = (uint64_t)(uint32_t) kernel_pd | PAGE_PRESENT;

	mapped_limit      = pd_needed * PD_COVERAGE;
	kernel_pd_entries = pd_needed;

	/* Kernel code and constants: present, not writable. */
	uint32_t text_start = (uint32_t) &kernel_text_start & ~(PAGE_SIZE - 1);
	uint32_t ro_end     = ((uint32_t) &kernel_rodata_end + PAGE_SIZE - 1)
	                      & ~(PAGE_SIZE - 1);

	for (uint32_t addr = text_start; addr < ro_end; addr += PAGE_SIZE) {
		uint64_t *entry = entry_for(addr);
		if (entry)
			*entry = (*entry & 0xFFFFF000ull) | PAGE_PRESENT;
	}

	isr_install_handler(14, page_fault_handler);

	/* CR4.PAE before CR3, because it changes how the CPU will read whatever
	 * CR3 points at. */
	uint32_t cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1u << 5);
	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	__asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pdpt));

	/* WP first, so the read-only bits above bind ring 0 too; then PG. */
	uint32_t cr0;
	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x00010000;
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

	__asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
	cr0 |= 0x80000000;
	__asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));
}

uint32_t paging_mapped_limit(void)
{
	return mapped_limit;
}

int paging_nx_available(void)
{
	return nx_supported;
}

void paging_make_user(uint32_t start, uint32_t end, int writable)
{
	uint64_t flags = PAGE_PRESENT | PAGE_USER;

	if (writable)
		flags |= PAGE_WRITE | PAGE_NX;   /* writable data is never executable */

	for (uint32_t addr = start & ~(PAGE_SIZE - 1); addr < end; addr += PAGE_SIZE) {
		uint64_t *pd = pd_for(active_pdpt, addr);
		if (pd)
			pd[(addr >> 21) & 0x1FF] |= PAGE_USER;

		paging_set_flags(addr, nx_supported ? flags : (flags & ~PAGE_NX));
	}
}

/* ---- address spaces ------------------------------------------------------- */

uint32_t paging_kernel_directory(void)
{
	return (uint32_t) kernel_pdpt;
}

uint32_t paging_create_address_space(void)
{
	/* One frame holds both the new PDPT and the new PD - the PDPT needs only
	 * 32 bytes, so putting it at the top of the frame and the PD below wastes
	 * nothing. They must not share a page table though, so the PD gets its
	 * own frame. */
	uint32_t pdpt_phys = pmm_alloc_frame();
	if (!pdpt_phys)
		return 0;

	uint32_t pd_phys = pmm_alloc_frame();
	if (!pd_phys) {
		pmm_free_frame(pdpt_phys);
		return 0;
	}

	uint64_t *pdpt = (uint64_t *) pdpt_phys;
	uint64_t *pd   = (uint64_t *) pd_phys;

	kmemset(pdpt, 0, PAGE_SIZE);
	kmemset(pd, 0, PAGE_SIZE);

	/* Copy the kernel's PD entries so the kernel is mapped identically here.
	 * The page TABLES are shared, not copied - a change to a kernel mapping is
	 * then visible in every address space automatically. Only the directory
	 * level is duplicated. */
	for (uint32_t i = 0; i < kernel_pd_entries; i++)
		pd[i] = kernel_pd[i];

	pdpt[0] = (uint64_t) pd_phys | PAGE_PRESENT;

	/* Entries 1-3 stay empty: this kernel never maps above 1 GB. */
	return pdpt_phys;
}

void paging_destroy_address_space(uint32_t pdpt_phys)
{
	if (!pdpt_phys || pdpt_phys == (uint32_t) kernel_pdpt)
		return;

	uint64_t *pdpt = (uint64_t *) pdpt_phys;

	if (pdpt[0] & PAGE_PRESENT) {
		uint32_t pd_phys = (uint32_t)(pdpt[0] & 0xFFFFF000ull);
		uint64_t *pd = (uint64_t *) pd_phys;

		/* Free only the page tables this address space added. The kernel's
		 * are shared by everyone and freeing one would take the system with
		 * it. */
		for (uint32_t i = kernel_pd_entries; i < TABLE_ENTRIES; i++)
			if (pd[i] & PAGE_PRESENT)
				pmm_free_frame((uint32_t)(pd[i] & 0xFFFFF000ull));

		pmm_free_frame(pd_phys);
	}

	pmm_free_frame(pdpt_phys);
}

void paging_switch(uint32_t pdpt_phys)
{
	uint64_t *pdpt = pdpt_phys ? (uint64_t *) pdpt_phys : kernel_pdpt;

	active_pdpt = pdpt;
	__asm__ volatile ("mov %0, %%cr3" : : "r"(pdpt) : "memory");
}
