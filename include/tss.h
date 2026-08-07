/* tss.h — Task State Segment.
 *
 * A structure the CPU reads automatically, and the reason it exists is one
 * specific problem: when ring 3 code triggers an interrupt, it is running on
 * the USER stack. The kernel cannot use that stack — a user program could point
 * it anywhere, including at kernel memory, and then the very act of handling
 * the interrupt would corrupt something.
 *
 * So on any privilege transition the CPU switches stacks, and it finds the
 * kernel stack address in the TSS. Two fields matter: ss0 and esp0. The rest is
 * legacy from an abandoned hardware task-switching scheme nobody uses.
 */
#ifndef ARTHIC_TSS_H
#define ARTHIC_TSS_H

#include <stdint.h>

void tss_install(uint32_t kernel_stack_top);

/* Set the kernel stack the CPU switches to on the next entry from ring 3.
 * With a scheduler this changes on every context switch — each task needs its
 * own kernel stack. */
void tss_set_kernel_stack(uint32_t stack_top);

#endif
