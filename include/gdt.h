/* gdt.h — Global Descriptor Table.
 *
 * Segment selectors. These are the values you load into segment registers,
 * and they are just byte offsets into the GDT: entry 1 sits 8 bytes in,
 * entry 2 sits 16 bytes in, and so on.
 *
 * The low 2 bits of a selector are the RPL (requested privilege level), which
 * is why the user selectors below have 3 OR'd into them. Ring 0 is the kernel,
 * ring 3 is user code. Rings 1 and 2 exist and essentially nobody uses them.
 */
#ifndef ARTHIC_GDT_H
#define ARTHIC_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08   /* entry 1, ring 0 */
#define GDT_KERNEL_DATA 0x10   /* entry 2, ring 0 */
#define GDT_USER_CODE   (0x18 | 3)   /* entry 3, ring 3 */
#define GDT_USER_DATA   (0x20 | 3)   /* entry 4, ring 3 */

void gdt_install(void);

#endif
