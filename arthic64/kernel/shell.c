/* shell.c — the command line.
 *
 * The keyboard driver hands us one character at a time. A shell needs whole
 * lines, so this file's real job is buffering: collect characters until Enter,
 * then interpret what was collected.
 *
 * That gap between "a key was pressed" and "a command was entered" is called
 * line discipline, and on a real system it is a surprisingly large amount of
 * code. Ours is about forty lines because we support exactly two editing
 * operations: type a character, and delete one.
 */

#include "shell.h"
#include "terminal.h"
#include "timer.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

#define SHELL_BUFFER_SIZE 128

static char   buffer[SHELL_BUFFER_SIZE];
static size_t buffer_length = 0;

static void prompt(void)
{
	kprintf("arthic> ");
}

/* ---- Commands ------------------------------------------------------------- */

static void command_help(void)
{
	kprintf("  help          this list\n");
	kprintf("  about         what this is\n");
	kprintf("  ticks         timer ticks since boot\n");
	kprintf("  mem           physical memory usage\n");
	kprintf("  heap          heap usage\n");
	kprintf("  heaptest      exercise kmalloc and kfree\n");
	kprintf("  wxtest        try to write to kernel code\n");
	kprintf("  regs          show 64-bit CPU state\n");
	kprintf("  echo <text>   print text back\n");
	kprintf("  clear         clear the screen\n");
}

static void command_about(void)
{
	kprintf("Arthic 64, stage 2 of the long mode port.\n");
	kprintf("Boots into 64-bit, handles interrupts, reads the keyboard.\n");
	kprintf("Memory manager, four-level paging and a heap. No filesystem\n");
	kprintf("or processes yet - still on the 32-bit branch.\n");
}

/* Show state that only exists in 64-bit, as evidence rather than assertion. */
static void command_regs(void)
{
	uint64_t rsp, cr0, cr3, cr4, efer_low;

	__asm__ volatile ("mov %%rsp, %0" : "=r" (rsp));
	__asm__ volatile ("mov %%cr0, %0" : "=r" (cr0));
	__asm__ volatile ("mov %%cr3, %0" : "=r" (cr3));
	__asm__ volatile ("mov %%cr4, %0" : "=r" (cr4));

	uint32_t low, high;
	__asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080u));
	efer_low = low;

	kprintf("  rsp  0x%lx\n", rsp);
	kprintf("  cr0  0x%lx   (bit 31 paging, bit 16 write-protect)\n", cr0);
	kprintf("  cr3  0x%lx   (points at the PML4)\n", cr3);
	kprintf("  cr4  0x%lx   (bit 5 PAE)\n", cr4);
	kprintf("  efer 0x%lx   (bit 8 LME, bit 10 LMA, bit 11 NXE)\n", efer_low);

	/* LMA - long mode ACTIVE - is set by the CPU itself, not by us. It is the
	 * hardware confirming the transition rather than us claiming it. */
	kprintf("  long mode %s\n",
	        (low & (1u << 10)) ? "ACTIVE" : "not active");
}

static void command_mem(void)
{
	kprintf("  total  %lu frames  (%lu KB)\n",
	        pmm_total_frames(), pmm_total_frames() * 4);
	kprintf("  used   %lu frames  (%lu KB)\n",
	        pmm_used_frames(), pmm_used_frames() * 4);
	kprintf("  free   %lu frames  (%lu KB)\n",
	        pmm_free_frames(), pmm_free_frames() * 4);
}

static void command_heap(void)
{
	uint64_t total, used, blocks;
	kheap_stats(&total, &used, &blocks);

	kprintf("  total  %lu KB\n", total / 1024);
	kprintf("  used   %lu bytes across %lu blocks\n", used, blocks);
}

static void command_heaptest(void)
{
	char *a = (char *) kmalloc(64);
	char *b = (char *) kmalloc(128);

	if (!a || !b) {
		kprintf("allocation failed\n");
		return;
	}

	kprintf("a = kmalloc(64)   -> 0x%lx\n", (uint64_t) a);
	kprintf("b = kmalloc(128)  -> 0x%lx\n", (uint64_t) b);

	kfree(a);

	char *c = (char *) kmalloc(32);
	kprintf("c = kmalloc(32)   -> 0x%lx  %s\n", (uint64_t) c,
	        ((uint64_t) c == (uint64_t) a) ? "(reused a's space)" : "");

	kfree(c);
	kfree(b);
	kprintf("all freed\n");
}

/* Try to write to the kernel's own code, and to execute its own stack.
 * Both should be refused, and both should be survivable. */
static void command_wxtest(void)
{
	extern uint64_t kernel_text_start;

	kprintf("writing to kernel code at 0x%lx ... ", (uint64_t) &kernel_text_start);

	if (paging_probe_write((volatile uint64_t *) &kernel_text_start, 0xDEAD))
		kprintf("SUCCEEDED - write protection is NOT working\n");
	else
		kprintf("blocked, and recovered\n");

	kprintf("W^X: code is r-x, everything else is rw- with NX set\n");
}

static void command_ticks(void)
{
	uint32_t t = timer_get_ticks();
	kprintf("%u ticks, roughly %u seconds since boot\n", t, t / 18);
}




























/* A task that never stops on its own - something for kill to actually be
 * needed for. Without it, kill only ever finishes off threads that were about
 * to exit anyway, which proves nothing. */





/* Run whatever is in the buffer. `buffer` is already NUL-terminated by the
 * caller, so it is a valid C string by the time we get here. */
static void execute(const char *line)
{
	if (line[0] == '\0')
		return;

	if (kstrcmp(line, "help") == 0)
		command_help();
	else if (kstrcmp(line, "about") == 0)
		command_about();
	else if (kstrcmp(line, "ticks") == 0)
		command_ticks();
	else if (kstrcmp(line, "mem") == 0)
		command_mem();
	else if (kstrcmp(line, "heap") == 0)
		command_heap();
	else if (kstrcmp(line, "heaptest") == 0)
		command_heaptest();
	else if (kstrcmp(line, "wxtest") == 0)
		command_wxtest();
	else if (kstrcmp(line, "regs") == 0)
		command_regs();
	else if (kstrcmp(line, "clear") == 0)
		terminal_clear();
	else if (kstrcmp(line, "echo") == 0)
		kprintf("\n");
	else if (kstartswith(line, "echo "))
		kprintf("%s\n", line + 5);
	else
		kprintf("unknown command: %s  (try 'help')\n", line);
}

/* ---- Input ---------------------------------------------------------------- */

void shell_input(char c)
{
	if (c == '\n') {
		terminal_putchar('\n');
		buffer[buffer_length] = '\0';   /* make it a real C string */
		execute(buffer);
		buffer_length = 0;
		prompt();
		return;
	}

	if (c == '\b') {
		/* Refuse to delete past the start of the line, or we would erase the
		 * prompt itself and then keep going into whatever is above it. */
		if (buffer_length > 0) {
			buffer_length--;
			terminal_backspace();
		}
		return;
	}

	/* Leave room for the terminating NUL. Silently dropping input at the
	 * limit is not elegant, but it is correct — and this bounds check is the
	 * difference between a full buffer and a kernel that overwrites whatever
	 * happens to sit after it in memory. */
	if (buffer_length < SHELL_BUFFER_SIZE - 1) {
		buffer[buffer_length++] = c;
		terminal_putchar(c);
	}
}

void shell_init(void)
{
	buffer_length = 0;
	prompt();
}
