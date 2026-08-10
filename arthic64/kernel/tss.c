/* tss.c - the 64-bit TSS.
 *
 * Almost the entire structure is dead weight kept for backward compatibility.
 * Three fields matter now: RSP0, which the CPU loads into RSP whenever an
 * interrupt or exception arrives from a less privileged ring; IST1, which
 * exists for exactly one exception - see the long comment on
 * DOUBLE_FAULT_STACK_SIZE below; and the I/O map base, which we use to deny
 * ring 3 all port access outright.
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

/* A double fault means the CPU already tried to deliver some OTHER
 * exception and failed partway through - almost always because the stack it
 * tried to push the exception frame onto was itself the problem, which is
 * exactly what happens when a guard page (kernel/task.c) catches a real
 * stack overflow: the page fault fires correctly, then pushing ITS OWN
 * exception frame onto that same, now-unmapped stack faults a second time,
 * and a fault during fault delivery is what vector 8 means.
 *
 * Ordinary exceptions use RSP0 for this, but RSP0 is exactly the stack that
 * just proved itself unusable. The IST exists for this one situation:
 * vector 8's gate (idt.c) is told, via ist=1, to load THIS stack
 * unconditionally, regardless of what RSP0 currently holds or whether it is
 * even mapped. Static rather than heap- or PMM-allocated deliberately - by
 * the time this stack is needed, something has already gone wrong enough
 * that trusting the normal allocators is exactly the assumption a
 * double-fault handler cannot afford to make. */
#define DOUBLE_FAULT_STACK_SIZE 4096
static uint8_t double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

void tss_install(uint64_t rsp0)
{
	kmemset(&tss, 0, sizeof(tss));

	tss.rsp0 = rsp0;

	/* ist[] is zero-indexed here (ist[0] is IST1) even though the field is
	 * conventionally called "IST1" - one more place a hardware structure's
	 * naming and its actual layout do not quite line up, worth stating
	 * plainly rather than leaving to be rediscovered by a wrong index. */
	tss.ist[0] = (uint64_t) double_fault_stack + DOUBLE_FAULT_STACK_SIZE;

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
