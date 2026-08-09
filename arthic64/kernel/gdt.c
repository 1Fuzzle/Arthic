/* gdt.c - the real 64-bit GDT, replacing boot.s's temporary one.
 *
 * WHY SEGMENTATION BARELY EXISTS HERE
 *
 * In long mode the CPU ignores base and limit for code and data segments
 * entirely - every segment behaves as though it spans all of memory, always.
 * What survives are a handful of flag bits: present, privilege level, and L
 * (64-bit code). So a descriptor is written as one raw 64-bit constant rather
 * than assembled field by field the way the 32-bit GDT did - most of those
 * fields no longer mean anything, and pretending otherwise would be noise.
 *
 * WHY USER DATA COMES BEFORE USER CODE
 *
 * SYSRET does not look up selectors by name. It takes one value from the STAR
 * MSR and adds fixed offsets: CS = value+16, SS = value+8. For that arithmetic
 * to land on our actual user descriptors, user code must sit exactly one
 * 8-byte slot after user data in the table. This is the CPU's requirement, not
 * a stylistic choice - the layout below is dictated by kernel/syscall.c and
 * cannot be reordered independently of it.
 *
 * THE TSS DESCRIPTOR IS DIFFERENT
 *
 * Every other entry here is 8 bytes. The TSS descriptor is 16, because it is a
 * system descriptor and its base address is now 64 bits wide - stored across
 * two GDT slots, filled in later once the TSS itself exists.
 */

#include "gdt.h"
#include "string.h"

/* 7 slots: null, kernel code, kernel data, user data, user code, then two for
 * the 16-byte TSS descriptor. */
static uint64_t gdt[7];

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct gdt_ptr gdt_pointer;

/* Fill in the two-slot TSS descriptor. Called by tss_install once the
 * structure it describes actually exists.
 *
 * Layout: low 8 bytes look like an ordinary system descriptor (limit, base
 * bits 0-23, access byte 0x89 = present, DPL 0, type 9 = available 64-bit
 * TSS, base bits 24-31). The high 8 bytes are just base bits 32-63, with the
 * rest reserved and zero - the format was widened without disturbing what
 * already existed, the same story as every other extended x86 structure.
 */
void gdt_set_tss(uint64_t base, uint32_t limit)
{
	uint64_t low = 0;
	low |= (uint64_t)(limit & 0xFFFF);
	low |= (base & 0xFFFFFFull) << 16;
	low |= 0x89ull << 40;
	low |= (uint64_t)((limit >> 16) & 0xF) << 48;
	low |= ((base >> 24) & 0xFFull) << 56;

	uint64_t high = (base >> 32) & 0xFFFFFFFFull;

	gdt[5] = low;
	gdt[6] = high;
}

void gdt_install(void)
{
	gdt[0] = 0;                              /* null descriptor, required */
	gdt[1] = 0x00AF9A000000FFFFull;          /* kernel code: P DPL0 L=1 exec */
	gdt[2] = 0x00CF92000000FFFFull;          /* kernel data: P DPL0 RW      */
	gdt[3] = 0x00CFF2000000FFFFull;          /* user data:   P DPL3 RW      */
	gdt[4] = 0x00AFFA000000FFFFull;          /* user code:   P DPL3 L=1     */
	gdt[5] = 0;                               /* TSS - filled by tss_install */
	gdt[6] = 0;

	gdt_pointer.limit = (uint16_t)(sizeof(gdt) - 1);
	gdt_pointer.base  = (uint64_t) &gdt;

	__asm__ volatile ("lgdt %0" : : "m" (gdt_pointer));

	/* CS is not reloaded with a far jump here, unlike the 32-bit version.
	 * That is safe specifically BECAUSE the new descriptor at selector 0x08
	 * has the same L bit and privilege level as the one boot.s already left
	 * loaded - the CPU only re-examines a segment's cached attributes on a
	 * control transfer, and nothing about CS is actually changing. A GDT
	 * that changed what selector 0x08 meant would need a real far jump. */
	__asm__ volatile (
		"mov $0x10, %%ax\n"
		"mov %%ax, %%ds\n"
		"mov %%ax, %%es\n"
		"mov %%ax, %%fs\n"
		"mov %%ax, %%ss\n"
		::: "ax"
	);
}
