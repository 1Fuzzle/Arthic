/* gdt.h - the 64-bit GDT.
 *
 * boot.s built a temporary 3-entry table just to reach long mode. This
 * replaces it with a real one: kernel and user segments, and a TSS descriptor.
 *
 * SELECTOR LAYOUT IS NOT ARBITRARY
 *
 * The SYSCALL/SYSRET instruction pair reads segment selectors out of the STAR
 * MSR by ADDING FIXED OFFSETS to a base value - it does not look them up by
 * name. That forces two things: user data must sit exactly one slot before
 * user code in the table, and the arithmetic below only comes out right for
 * this specific layout. Get the order wrong and SYSRET returns to the wrong
 * ring with the wrong stack segment, silently.
 */
#ifndef ARTHIC_GDT_H
#define ARTHIC_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   (0x18 | 3)   /* 0x1B - deliberately BEFORE user code */
#define GDT_USER_CODE   (0x20 | 3)   /* 0x23 */
#define GDT_TSS         0x28

void gdt_install(void);
void gdt_set_tss(uint64_t base, uint32_t limit);

#endif
