/* main.c - kernel_main, now in 64-bit.
 *
 * Reached from boot.s after the mode switch. Note the arguments: the multiboot
 * magic and info pointer arrive in rdi and rsi because that is where the 64-bit
 * calling convention puts the first two, and boot.s put them there deliberately
 * before the mode change.
 */

#include <stdint.h>

#include "terminal.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "shell.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "gdt.h"
#include "tss.h"
#include "syscall.h"
#include "usermode.h"
#include "task.h"

void kernel_main(uint32_t magic, struct multiboot_info *mbi)
{

	terminal_initialise();

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_CYAN, VGA_BLACK));
	terminal_write("  _   _   _   _   _   _\n");
	terminal_write(" / \\ / \\ / \\ / \\ / \\ / \\\n");
	terminal_write("( A | r | t | h | i | c )\n");
	terminal_write(" \\_/ \\_/ \\_/ \\_/ \\_/ \\_/\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREY, VGA_BLACK));
	terminal_write("Arthic 64 - stage 5\n");
	terminal_write("Running in 64-bit long mode.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_LIGHT_GREEN, VGA_BLACK));

	if (magic != 0x2BADB002u)
		kprintf("warning: unexpected multiboot magic 0x%x\n", magic);

	idt_install();
	kprintf("IDT installed: 16-byte gates, 32 exceptions, 16 IRQs.\n");

	/* Proof we are genuinely in 64-bit rather than merely believing so.
	 * A 32-bit build cannot hold this value in a register at all, and %lx is
	 * the format that reads all of it. */
	uint64_t wide = 0x0123456789ABCDEFull;
	kprintf("64-bit registers: 0x%lx\n", wide);

	pmm_init(mbi);
	kprintf("PMM installed: %lu KB usable, %lu KB in use.\n",
	        pmm_free_frames() * 4, pmm_used_frames() * 4);

	paging_init();
	kprintf("Paging: four levels, %lu MB in 4 KB pages, W^X enforced.\n",
	        paging_mapped_limit() / (1024 * 1024));

	kheap_init();
	{
		uint64_t heap_total;
		kheap_stats(&heap_total, 0, 0);
		kprintf("Heap ready: %lu KB.\n", heap_total / 1024);
	}

	uint64_t cs;
	__asm__ volatile ("mov %%cs, %0" : "=r" (cs));
	kprintf("code selector 0x%lx, L bit set.\n", cs);

	gdt_install();
	kprintf("GDT installed: kernel + user segments, TSS slot ready.\n");

	{
		uint64_t rsp;
		__asm__ volatile ("mov %%rsp, %0" : "=r" (rsp));
		tss_install(rsp - 512);
	}
	syscall_install();
	usermode_init();
	task_init();
	kprintf("TSS installed. SYSCALL/SYSRET armed via STAR/LSTAR/FMASK.\n");
	kprintf("Scheduler running: preemptive round robin on the timer.\n\n");

	terminal_set_colour(vga_entry_colour(VGA_WHITE, VGA_BLACK));
	kprintf("Interrupts enabled. Keyboard live. Type 'help'.\n\n");

	timer_install();
	keyboard_install(shell_input);

	__asm__ volatile ("sti");

	shell_init();

	for (;;)
		__asm__ volatile ("hlt");
}
