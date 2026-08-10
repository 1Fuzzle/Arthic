/* cpuprot.c - enabling SMEP and SMAP.
 *
 * Both are feature-gated: neither existed before roughly 2012 (SMEP, Ivy
 * Bridge) or 2015 (SMAP, Broadwell), and QEMU's default CPU model does not
 * advertise either unless asked. Detected via CPUID leaf 7, the same
 * mechanism NX's availability was checked with on the 32-bit branch - ask the
 * hardware, do not assume.
 */

#include "cpuprot.h"
#include "terminal.h"
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

	uint64_t cr4;
	__asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

	if (smep_ok)
		cr4 |= (1u << 20);   /* CR4.SMEP */
	if (smap_ok)
		cr4 |= (1u << 21);   /* CR4.SMAP */

	__asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

	/* SMAP defaults AC to clear - reads/writes to user pages from ring 0 are
	 * blocked until a STAC explicitly and temporarily lifts that, exactly
	 * where the kernel means to touch ring-3 memory on purpose. Nothing
	 * further to do here; every such site brackets itself. */

	return smep_ok && smap_ok;
}
