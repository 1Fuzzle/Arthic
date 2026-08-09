/* tss.c - the 64-bit TSS.
 *
 * Almost the entire structure is dead weight kept for backward compatibility.
 * Two fields matter: RSP0, which the CPU loads into RSP whenever an interrupt
 * or exception arrives from a less privileged ring, and the I/O map base,
 * which we use to deny ring 3 all port access outright.
 */

#include "tss.h"
#include "gdt.h"
#include "string.h"

/* Genuinely 104 bytes in the real specification, but `packed` is what makes
 * that guaranteed rather than assumed - the same reasoning as every other
 * hardware-read structure in this kernel. */
struct tss_entry {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

static struct tss_entry tss;

void tss_install(uint64_t rsp0)
{
	kmemset(&tss, 0, sizeof(tss));

	tss.rsp0 = rsp0;

	/* Same decision as the 32-bit kernel, for the same reason: pointing the
	 * I/O permission bitmap past the end of the structure means "no bitmap",
	 * which the CPU reads as ring 3 having no port access whatsoever. A user
	 * program that wants hardware access must ask through a syscall, where the
	 * kernel decides. */
	tss.iomap_base = sizeof(tss);

	gdt_set_tss((uint64_t) &tss, sizeof(tss));

	/* ltr takes the TSS selector, not its address - the address already
	 * lives inside the GDT descriptor we just filled in. */
	__asm__ volatile ("ltr %0" : : "r" ((uint16_t) GDT_TSS));
}

void tss_set_kernel_stack(uint64_t rsp0)
{
	tss.rsp0 = rsp0;
}
