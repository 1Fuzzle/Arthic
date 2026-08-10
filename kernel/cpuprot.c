/* cpuprot.c - enabling SMEP and SMAP, 32-bit.
 *
 * CPUID leaf 7 gives the same feature bits regardless of whether the CPU is
 * currently running in 32-bit or 64-bit mode - this is a property of the
 * hardware, not of what mode asked. QEMU's default 32-bit CPU model does not
 * advertise either feature any more than its 64-bit one does, so the run
 * command needs to ask explicitly, the same situation NX was already in.
 */

#include "cpuprot.h"
#include <stdint.h>

static int smep_ok = 0;
static int smap_ok = 0;

static void cpuid7(uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
	uint32_t eax_out;

	__asm__ volatile ("cpuid"
	                  : "=a"(eax_out), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
	                  : "a"(7), "c"(0));
}

int cpuprot_smep_available(void) { return smep_ok; }
int cpuprot_smap_available(void) { return smap_ok; }

int cpuprot_init(void)
{
	uint32_t ebx, ecx, edx;
	cpuid7(&ebx, &ecx, &edx);

	smep_ok = (ebx & (1u << 7))  != 0;   /* CPUID.7.0.EBX bit 7  */
	smap_ok = (ebx & (1u << 20)) != 0;   /* CPUID.7.0.EBX bit 20 */

	uint32_t cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

	if (smep_ok)
		cr4 |= (1u << 20);   /* CR4.SMEP */
	if (smap_ok)
		cr4 |= (1u << 21);   /* CR4.SMAP */

	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	return smep_ok && smap_ok;
}
