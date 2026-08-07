/* main.c — kernel_main, the C entry point.
 *
 * boot.s calls this. Its only job is to bring subsystems up in the right order
 * and then hand control to the shell. Nothing else belongs here — if this file
 * starts growing, whatever was added wants its own file.
 */

#include <stdint.h>

#include "terminal.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "tss.h"
#include "syscall.h"
#include "usermode.h"
#include "task.h"
#include "multiboot.h"

/* ---- Entry point ----------------------------------------------------------
 * boot.s calls this. Note it never returns — an OS kernel has nothing to
 * return to.
 */
void kernel_main(uint32_t magic, struct multiboot_info *mbi) {
	terminal_initialise();

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_CYAN, VGA_BLACK));
	terminal_write("  _   _   _   _   _   _\n");
	terminal_write(" / \\ / \\ / \\ / \\ / \\ / \\\n");
	terminal_write("( A | r | t | h | i | c )\n");
	terminal_write(" \\_/ \\_/ \\_/ \\_/ \\_/ \\_/\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK));
	terminal_write("Arthic kernel v1.6\n");
	terminal_write("Booted in 32-bit protected mode.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_DARK_GREY, VGA_BLACK));
	terminal_write("No scheduler. No memory manager. No drivers.\n");
	terminal_write("Just this. Everything else is yours to add.\n\n");

	/* Print enough lines to push the banner off the top, proving the scroll
	 * works.
	 *
	 * Note `char line[]` and not `const char *line`. That difference is real:
	 * a char array is our own writable COPY of those bytes, so line[5] = c
	 * edits it. Declared as a pointer it would point at read-only memory and
	 * writing through it would be undefined behaviour. Same-looking text,
	 * completely different thing.                                          */
	/* Replace GRUB's borrowed descriptor table with our own before doing
	 * anything else. Nothing visible happens — that is the point. */
	gdt_install();
	idt_install();

	/* Confirm we were actually loaded by a multiboot loader before trusting
	 * the pointer it supposedly left us. */
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		terminal_set_colour(vga_entry_colour(VGA_LIGHT_RED, VGA_BLACK));
		kprintf("bad multiboot magic: 0x%x - refusing to continue\n", magic);
		for (;;)
			__asm__ volatile ("cli; hlt");
	}

	pmm_init(mbi);
	paging_init();
	kheap_init();

	/* The TSS needs a kernel stack address; usermode_run refines it per
	 * entry, so any sane value will do here. */
	{
		uint32_t esp;
		__asm__ volatile ("mov %%esp, %0" : "=r" (esp));
		tss_install(esp);
	}

	syscall_install();
	usermode_init();
	task_init();

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREEN, VGA_BLACK));
	kprintf("GDT installed: 5 entries, flat model, ring 0 + ring 3 ready.\n");
	kprintf("IDT installed: 32 exception handlers, 16 IRQs, PIC remapped.\n");
	kprintf("PMM  installed: %u KB usable, %u KB in use.\n",
	        pmm_free_frames() * 4, pmm_used_frames() * 4);
	kprintf("Paging enabled: %u MB mapped, kernel code read-only.\n",
	        paging_mapped_limit() / (1024 * 1024));
	{
		uint32_t heap_total;
		kheap_stats(&heap_total, 0, 0);
		kprintf("Heap  ready: %u KB, first-fit with coalescing.\n",
		        heap_total / 1024);
	}
	kprintf("TSS + ring 3 ready. Syscall gate at int 0x80, DPL 3.\n");
	kprintf("Scheduler running: preemptive round robin, with sleeping.\n");
	kprintf("kprintf is alive.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK));
	kprintf("  decimal      %d\n", 1234);
	kprintf("  negative     %d\n", -4321);
	kprintf("  zero         %d\n", 0);
	kprintf("  int minimum  %d\n", (int32_t) 0x80000000);
	kprintf("  unsigned     %u\n", 4294967295u);
	kprintf("  hex          0x%x\n", 0xDEADBEEFu);
	kprintf("  string       %s\n", "Arthic");
	kprintf("  character    %c\n", 'A');
	kprintf("  percent      100%%\n");
	kprintf("  mixed        %s v%d.%d at 0x%x\n", "kernel", 1, 6, 0xB8000u);

	terminal_set_colour(vga_entry_colour(VGA_WHITE, VGA_BLACK));
	kprintf("\nInterrupts enabled. Keyboard live. Type 'help'.\n\n");

	timer_install();
	keyboard_install(shell_input);

	/* Want to see the exception handler work? Uncomment this. It divides by
	 * zero on purpose, which raises exception 0 and halts with a register
	 * dump. Comment it out again afterwards — it is fatal by design. */
	/* { volatile int a = 1, b = 0; kprintf("%d", a / b); } */

	/* sti — set interrupt flag. Until this instruction the CPU has been
	 * ignoring all hardware interrupts. This is the moment Arthic stops
	 * being a program that runs top to bottom and becomes a system that
	 * responds to the outside world. */
	__asm__ volatile ("sti");

	shell_init();

	/* Idle forever. hlt stops the CPU until the next interrupt arrives,
	 * which is why this loop uses no power — unlike a bare while(1), which
	 * would spin a core at 100% doing nothing. */
	for (;;)
		__asm__ volatile ("hlt");

}
