/* idt.h — Interrupt Descriptor Table. */
#ifndef ARTHIC_IDT_H
#define ARTHIC_IDT_H

#include <stdint.h>

/* The CPU state at the moment an interrupt fired.
 *
 * THE ORDER OF THESE FIELDS IS NOT A STYLE CHOICE. It mirrors, exactly, the
 * order things were pushed onto the stack by interrupts.s — reading top of
 * struct to bottom is reading stack from low address to high.
 *
 *   ds                       pushed by our stub
 *   edi..eax                 pushed by `pusha`, in the order pusha uses
 *   int_no, err_code         pushed by our stub
 *   eip, cs, eflags          pushed by the CPU itself
 *   useresp, ss              pushed by the CPU only on a privilege change
 *
 * Change one side without the other and C reads garbage, silently. This kind
 * of implicit contract between assembly and C is normal in kernels and is
 * exactly why the comment is here.
 */
struct registers {
	uint32_t ds;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
	uint32_t int_no, err_code;
	uint32_t eip, cs, eflags, useresp, ss;
};

/* An IRQ handler is any function taking the saved state. */
typedef void (*irq_handler_t)(struct registers *regs);

void idt_install(void);
void irq_install_handler(int irq, irq_handler_t handler);

#endif
