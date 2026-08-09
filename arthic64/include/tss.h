/* tss.h - the 64-bit Task State Segment.
 *
 * AMD deleted hardware task switching when it designed long mode, so this
 * structure no longer holds a "task state" in any real sense - it is just a
 * short list of stack pointers the CPU reads automatically on certain
 * transitions.
 *
 * RSP0 IS FOR INTERRUPTS. IT IS NOT FOR SYSCALL.
 *
 * That distinction matters and is easy to miss. When a ring 3 program takes an
 * interrupt or exception - a page fault, the timer, a bad instruction - the
 * CPU switches to RSP0 automatically, the same as 32-bit's ESP0. But the
 * SYSCALL instruction does NOT consult the TSS at all. It leaves RSP exactly
 * where the user program left it, and it is the kernel's job to move to a safe
 * stack itself - which is what swapgs in kernel/syscall.c is for. Two separate
 * mechanisms, two separate stacks.
 *
 * THE IST
 *
 * Seven more stack pointers, IST1-IST7. A specific interrupt vector can be
 * told (via its IDT gate) to always switch to one of these regardless of what
 * RSP0 currently is. The point is a fault whose own stack pointer is the thing
 * that broke - a double fault caused by a corrupted kernel stack cannot safely
 * use that same stack to report itself. We leave all seven at zero for now;
 * wiring double-fault onto a dedicated IST entry is a small, valuable addition
 * once the kernel does more that could corrupt its own stack.
 */
#ifndef ARTHIC_TSS_H
#define ARTHIC_TSS_H

#include <stdint.h>

void tss_install(uint64_t rsp0);
void tss_set_kernel_stack(uint64_t rsp0);

#endif
