/* gdt.c — building and loading the Global Descriptor Table.
 *
 * WHAT THIS IS FOR
 *
 * x86 has an old memory-protection scheme called segmentation. Every memory
 * access goes through a "descriptor" that says where a region starts, how big
 * it is, and what privilege level may touch it. Modern systems barely use it —
 * paging does that job far better — but the CPU still REQUIRES a valid
 * descriptor table to exist. So the universal approach is to define segments
 * that span the entire 4 GB address space, which effectively switches
 * segmentation off, and then rely on paging later. That arrangement is
 * conventionally called the "flat model".
 *
 * GRUB set up a temporary GDT to get us into protected mode and we have been
 * borrowing it. It works, but it is not ours, we cannot extend it, and we have
 * no guarantee about where it lives in memory. Interrupt handlers need a code
 * selector we control, so we build our own now.
 *
 * The one part that IS about protection: each descriptor carries a DPL —
 * descriptor privilege level, 0 for kernel, 3 for user. That field is the
 * hardware mechanism behind the entire kernel/userspace boundary. We define
 * ring 3 entries here even though nothing uses them yet, so the table does not
 * need rebuilding when user mode arrives.
 */

#include "gdt.h"
#include <stdint.h>

/* A single 8-byte descriptor.
 *
 * The layout is genuinely awful: the base address is split across three
 * non-adjacent fields and the limit across two, because Intel extended a
 * 16-bit design without breaking old software. You do not need to remember it,
 * only to fill it in correctly once.
 *
 * __attribute__((packed)) is essential. Without it the compiler is free to
 * insert padding between fields for alignment, and the CPU reads this
 * structure directly — a single byte of padding would shift everything after
 * it and the descriptor would be garbage. Any struct describing a hardware or
 * on-disk layout needs this.
 */
struct gdt_entry {
	uint16_t limit_low;    /* limit bits 0-15                        */
	uint16_t base_low;     /* base bits 0-15                         */
	uint8_t  base_middle;  /* base bits 16-23                        */
	uint8_t  access;       /* present, DPL, type flags               */
	uint8_t  granularity;  /* limit bits 16-19 + size/granularity    */
	uint8_t  base_high;    /* base bits 24-31                        */
} __attribute__((packed));

/* What we hand to the CPU: the table's size and address.
 *
 * `limit` is the size in bytes MINUS ONE. That off-by-one is deliberate in the
 * hardware design, not a mistake — it lets a limit of 0xFFFF describe a full
 * 65536-byte table. Getting it wrong is a classic bug.
 */
struct gdt_ptr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdt_pointer;

/* Fill in one descriptor, scattering base and limit into their fields.
 *
 * The shifts and masks are pure bit surgery: >> 16 moves bits 16-23 down into
 * a byte, & 0xFF keeps only the low 8 bits, & 0x0F keeps only the low nibble.
 * Nothing conceptually deep, just reassembling a value the shape the hardware
 * wants it.
 */
static void gdt_set_gate(int index, uint32_t base, uint32_t limit,
                         uint8_t access, uint8_t granularity)
{
	gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
	gdt[index].base_middle = (uint8_t)((base >> 16) & 0xFF);
	gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

	gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
	gdt[index].granularity = (uint8_t)((limit >> 16) & 0x0F);
	gdt[index].granularity |= (granularity & 0xF0);

	gdt[index].access      = access;
}

/* Load the table and switch to the new segments.
 *
 * lgdt tells the CPU where the table is, but segment registers keep using
 * their OLD descriptors until they are reloaded. Data registers can simply be
 * assigned. CS cannot — there is no `mov %ax, %cs` instruction. The only way
 * to change it is a FAR JUMP, which sets CS and EIP together.
 *
 * So `ljmp $0x08, $1f` means "jump to label 1, and while you are at it load
 * selector 0x08 into CS". The destination is the very next instruction, so
 * nothing moves; the jump exists purely for its side effect. This is one of
 * the odder idioms in x86 and every kernel contains it.
 *
 * "memory" in the clobber list warns the compiler that this asm affects memory
 * in ways it cannot see, so it must not cache or reorder around it.
 */
static void gdt_flush(uint32_t pointer_address)
{
	__asm__ volatile (
		"lgdt (%0)          \n"
		"ljmp $0x08, $1f    \n"   /* reload CS */
		"1:                 \n"
		"mov $0x10, %%ax    \n"   /* kernel data selector */
		"mov %%ax, %%ds     \n"
		"mov %%ax, %%es     \n"
		"mov %%ax, %%fs     \n"
		"mov %%ax, %%gs     \n"
		"mov %%ax, %%ss     \n"
		:
		: "r" (pointer_address)
		: "eax", "memory"
	);
}

void gdt_install(void)
{
	gdt_pointer.limit = (uint16_t)(sizeof(gdt) - 1);
	gdt_pointer.base  = (uint32_t)&gdt;

	/* Entry 0 must be all zeroes. The CPU reserves it as the "null
	 * descriptor" and faults if you ever load it — which is deliberate, it
	 * catches code that uses an uninitialised segment register. */
	gdt_set_gate(0, 0, 0, 0, 0);

	/* Access byte, bit by bit:
	 *   bit 7 present        1 = this descriptor is valid
	 *   bits 6-5 DPL         00 = ring 0, 11 = ring 3
	 *   bit 4 type           1 = code or data (0 = system descriptor)
	 *   bit 3 executable     1 = code segment, 0 = data segment
	 *   bit 2 direction      0 = grows up
	 *   bit 1 rw             code: readable, data: writable
	 *   bit 0 accessed       CPU sets this itself
	 *
	 * Granularity byte:
	 *   0xCF = 4 KiB granularity + 32-bit + limit bits 16-19 all set.
	 *   With 4 KiB granularity a limit of 0xFFFFF means 0xFFFFF * 4096, i.e.
	 *   the full 4 GB. That is what makes these segments "flat".
	 */
	gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xCF);   /* kernel code, ring 0 */
	gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF);   /* kernel data, ring 0 */
	gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xCF);   /* user code,   ring 3 */
	gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xCF);   /* user data,   ring 3 */

	gdt_flush((uint32_t)&gdt_pointer);
}
