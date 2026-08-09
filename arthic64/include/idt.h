/* idt.h - the Interrupt Descriptor Table, 64-bit.
 *
 * Structurally the same idea as the 32-bit version, but every gate is 16 bytes
 * instead of 8, because a handler address is now 64 bits wide.
 */
#ifndef ARTHIC_IDT_H
#define ARTHIC_IDT_H

#include <stdint.h>

/* The CPU state at the moment an interrupt fired.
 *
 * THE ORDER MIRRORS interrupts.s EXACTLY - reading top to bottom is reading the
 * stack from low address to high. Change one side without the other and C reads
 * garbage, silently.
 *
 * There is no `pusha` in 64-bit mode; AMD removed it. So the stub pushes all
 * fifteen registers by hand, and this struct lists them in the order they land.
 */
struct registers {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t int_no, err_code;

	/* Pushed by the CPU. Note that rsp and ss are ALWAYS pushed in long mode,
	 * even without a privilege change - a difference from 32-bit, where they
	 * appeared only when the ring changed. One fewer special case. */
	uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct registers *regs);

void idt_install(void);
void isr_install_handler(int exception, irq_handler_t handler);
void irq_install_handler(int irq, irq_handler_t handler);
void irq_unmask(uint8_t irq);

#endif
