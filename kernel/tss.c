/* tss.c — the Task State Segment.
 *
 * Ring 3 code runs on its own stack. When an interrupt or syscall arrives, the
 * kernel must not keep using it: a user program controls that pointer and could
 * aim it at kernel memory, at unmapped memory, or simply at something too small
 * to hold an interrupt frame. Any of those turns interrupt handling into a
 * vulnerability.
 *
 * The CPU therefore switches stacks automatically on entry from a less
 * privileged ring, and it reads the stack to switch TO from this structure.
 * That is the whole job. Everything else in here is vestigial — Intel designed
 * the TSS for hardware task switching, which turned out to be slower than doing
 * it in software, so every modern OS uses exactly two of these fields.
 */

#include "tss.h"
#include "gdt.h"
#include "string.h"

struct tss_entry {
	uint32_t prev_tss;
	uint32_t esp0;        /* kernel stack pointer  <- we use this */
	uint32_t ss0;         /* kernel stack segment  <- and this    */
	uint32_t esp1, ss1, esp2, ss2;
	uint32_t cr3, eip, eflags;
	uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
	uint32_t es, cs, ss, ds, fs, gs;
	uint32_t ldt;
	uint16_t trap;
	uint16_t iomap_base;
} __attribute__((packed));

static struct tss_entry tss;

void tss_install(uint32_t kernel_stack_top)
{
	kmemset(&tss, 0, sizeof(tss));

	tss.ss0  = GDT_KERNEL_DATA;
	tss.esp0 = kernel_stack_top;

	/* Point the I/O permission bitmap past the end of the structure. That
	 * means "no bitmap", which the CPU reads as "ring 3 may not use in or out
	 * instructions at all".
	 *
	 * This is a real security decision and worth being deliberate about. With
	 * port access, a user program could talk to the disk controller directly
	 * and bypass every check the kernel makes. Denying it wholesale is the
	 * right default; anything needing port access can go through a syscall. */
	tss.iomap_base = sizeof(tss);

	gdt_set_tss((uint32_t) &tss, sizeof(tss));

	/* ltr loads the task register with the TSS selector. Until this runs the
	 * CPU does not know the structure exists. */
	__asm__ volatile ("ltr %0" : : "r" ((uint16_t) GDT_TSS));
}

void tss_set_kernel_stack(uint32_t stack_top)
{
	tss.esp0 = stack_top;
}
