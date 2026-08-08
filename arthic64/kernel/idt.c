/* idt.c — the Interrupt Descriptor Table, the PIC, and the C-side handlers.
 *
 * The IDT is structurally the same idea as the GDT: a table of descriptors you
 * hand to the CPU. The difference is what it describes. Each of its 256 entries
 * says "if interrupt N happens, jump here, in this code segment, at this
 * privilege level."
 */

#include "idt.h"
#include "io.h"
#include "terminal.h"

/* One 8-byte gate descriptor. Same awkward split-field style as the GDT —
 * the handler address lives in two halves at opposite ends of the struct. */
/* A 64-bit gate is 16 bytes, not 8 - the handler address is now 64 bits and no
 * longer fits in two halves at opposite ends of an 8-byte descriptor. It is
 * split into three pieces instead, for the same historical reason: the format
 * was extended without moving the fields that already existed.
 *
 * `ist` is new and genuinely useful: a non-zero value tells the CPU to switch
 * to a known-good stack from the TSS for this interrupt, regardless of what the
 * current one looks like. That is how a real kernel survives a fault caused by
 * a broken stack pointer. We leave it zero for now. */
struct idt_entry {
	uint16_t base_low;     /* handler address bits 0-15   */
	uint16_t selector;     /* which code segment          */
	uint8_t  ist;          /* interrupt stack table index */
	uint8_t  flags;        /* present, DPL, gate type     */
	uint16_t base_mid;     /* bits 16-31                  */
	uint32_t base_high;    /* bits 32-63                  */
	uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idt_pointer;

/* Registered handlers for the 16 hardware IRQs. NULL means "nobody cares
 * about this one yet", which is fine — we still have to acknowledge it. */
static irq_handler_t irq_handlers[16];
static irq_handler_t isr_handlers[32];

/* From interrupts.s. `extern` means "this exists, defined elsewhere". */
extern void idt_flush(void *);
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

/* Human-readable names, indexed by exception number. When something goes
 * wrong you want to read "General Protection Fault", not "exception 13". */
static const char *exception_names[32] = {
	"Divide By Zero",           "Debug",
	"Non-Maskable Interrupt",   "Breakpoint",
	"Overflow",                 "Bound Range Exceeded",
	"Invalid Opcode",           "Device Not Available",
	"Double Fault",             "Coprocessor Segment Overrun",
	"Invalid TSS",              "Segment Not Present",
	"Stack-Segment Fault",      "General Protection Fault",
	"Page Fault",               "Reserved",
	"x87 Floating-Point",       "Alignment Check",
	"Machine Check",            "SIMD Floating-Point",
	"Virtualization",           "Control Protection",
	"Reserved", "Reserved", "Reserved", "Reserved",
	"Reserved", "Reserved",
	"Hypervisor Injection",     "VMM Communication",
	"Security Exception",       "Reserved"
};

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t selector,
                         uint8_t flags)
{
	idt[num].base_low  = (uint16_t)(base & 0xFFFF);
	idt[num].base_mid  = (uint16_t)((base >> 16) & 0xFFFF);
	idt[num].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
	idt[num].selector  = selector;
	idt[num].ist       = 0;
	idt[num].flags     = flags;
	idt[num].reserved  = 0;
}

/* ---- The PIC --------------------------------------------------------------
 * The Programmable Interrupt Controller is the chip that hardware devices
 * signal through. There are two of them chained together, giving 16 IRQ lines.
 *
 * Out of the box they deliver IRQs 0-15 as interrupt vectors 0-15 — which
 * collides head-on with the CPU's own exceptions, also numbered 0-15. A timer
 * tick would arrive looking exactly like a divide-by-zero. That was a
 * reasonable decision in 1981 and a disaster ever since.
 *
 * The fix, which every x86 OS performs, is to reprogram the PICs to deliver
 * their interrupts at 32-47 instead, safely past the reserved range.
 */
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20   /* "end of interrupt" acknowledgement */

static void pic_remap(void)
{
	/* Start the initialisation sequence. After this the chips expect
	 * exactly four data bytes, in a fixed order. */
	outb(PIC1_COMMAND, 0x11); io_wait();
	outb(PIC2_COMMAND, 0x11); io_wait();

	outb(PIC1_DATA, 0x20); io_wait();   /* master starts at vector 32 */
	outb(PIC2_DATA, 0x28); io_wait();   /* slave starts at vector 40  */

	outb(PIC1_DATA, 0x04); io_wait();   /* slave is wired to IRQ line 2 */
	outb(PIC2_DATA, 0x02); io_wait();   /* slave's identity on that line */

	outb(PIC1_DATA, 0x01); io_wait();   /* 8086 mode */
	outb(PIC2_DATA, 0x01); io_wait();

	/* Mask everything to begin with. A device whose driver does not exist
	 * yet should not be allowed to interrupt us. Unmask deliberately, one
	 * line at a time, as each driver is written. */
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}

/* Allow one IRQ line through. Each bit of the mask register disables one line,
 * so we clear the bit rather than writing the whole byte — read-modify-write
 * again, for the same reason as the cursor register. */
void irq_unmask(uint8_t irq)
{
	uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	if (irq >= 8) irq -= 8;
	outb(port, (uint8_t)(inb(port) & ~(1 << irq)));
}


void isr_install_handler(int exception, irq_handler_t handler)
{
	if (exception < 0 || exception > 31) return;
	isr_handlers[exception] = handler;
}

void irq_install_handler(int irq, irq_handler_t handler)
{
	if (irq < 0 || irq > 15) return;
	irq_handlers[irq] = handler;
	irq_unmask((uint8_t)irq);
}

/* ---- Handlers -------------------------------------------------------------
 * Called from the assembly stubs. Note these are not `static`: the assembly
 * has to be able to find them by name at link time.
 */

/* A CPU exception. These mean something went genuinely wrong.
 *
 * We print everything we know and stop. Halting on an unexpected fault is the
 * safe choice — continuing in an unknown state is how a bug becomes an
 * exploit. Later, page faults will be handled and resumed rather than fatal,
 * because they are a normal part of demand paging.
 */
void isr_handler(struct registers *regs)
{
	/* A registered handler gets first refusal. Page faults are handled and
	 * resumed in a real system; only unclaimed exceptions are fatal. */
	if (regs->int_no < 32 && isr_handlers[regs->int_no]) {
		isr_handlers[regs->int_no](regs);
		return;
	}


	const char *name = (regs->int_no < 32)
		? exception_names[regs->int_no] : "Unknown";

	kprintf("\n*** EXCEPTION %d: %s\n", (int32_t) regs->int_no, name);
	kprintf("    rip=0x%lx  cs=0x%lx  rflags=0x%lx  err=0x%lx\n",
	        regs->rip, regs->cs, regs->rflags, regs->err_code);
	kprintf("    rax=0x%lx  rbx=0x%lx  rcx=0x%lx  rdx=0x%lx\n",
	        regs->rax, regs->rbx, regs->rcx, regs->rdx);
	kprintf("    system halted.\n");

	for (;;)
		__asm__ volatile ("cli; hlt");
}

/* A hardware interrupt. Normal traffic, not an error. */
void irq_handler(struct registers *regs)
{
	uint64_t irq = regs->int_no - 32;

	if (irq < 16 && irq_handlers[irq])
		irq_handlers[irq](regs);

	/* Acknowledge it, or the PIC will never send another. The slave must be
	 * told first when the interrupt came from it — miss this and IRQs 8-15
	 * fire exactly once and then go silent forever. Classic bug. */
	if (irq >= 8)
		outb(PIC2_COMMAND, PIC_EOI);
	outb(PIC1_COMMAND, PIC_EOI);

}

void idt_install(void)
{
	idt_pointer.limit = (uint16_t)(sizeof(idt) - 1);
	idt_pointer.base  = (uint64_t)&idt;

	for (int i = 0; i < 256; i++)
		idt_set_gate((uint8_t)i, 0, 0, 0);

	for (int i = 0; i < 16; i++)
		irq_handlers[i] = 0;

	for (int i = 0; i < 32; i++)
		isr_handlers[i] = 0;


	pic_remap();

	/* flags 0x8E = present, DPL 0, 32-bit interrupt gate.
	 *
	 * DPL 0 is a security decision: ring 3 code cannot invoke these
	 * deliberately with an `int` instruction. When we add a syscall gate
	 * later it will be the one deliberate exception, with DPL 3.
	 *
	 * "Interrupt gate" rather than "trap gate" means the CPU clears the
	 * interrupt flag on entry, so a handler is not itself interrupted
	 * halfway through. */
	idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
	idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
	idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
	idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
	idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
	idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
	idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
	idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
	idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
	idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
	idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
	idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
	idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
	idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
	idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
	idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
	idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
	idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
	idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
	idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
	idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
	idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
	idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
	idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
	idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
	idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
	idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
	idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
	idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
	idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
	idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
	idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

	idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);
	idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
	idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);
	idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
	idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);
	idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
	idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);
	idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
	idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);
	idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
	idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
	idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
	idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
	idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
	idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
	idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

	/* The syscall gate, and the ONLY entry with DPL 3.
	 *
	 * 0xEE = present, DPL 3, 32-bit interrupt gate. That DPL is what lets ring
	 * 3 execute `int $0x80` without a general protection fault. Every other
	 * gate is DPL 0 precisely so user code cannot invoke handlers directly. */

	idt_flush(&idt_pointer);
}
