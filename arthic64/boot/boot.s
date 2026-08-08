/* boot.s - from GRUB's 32-bit handoff into 64-bit long mode.
 *
 * GRUB always starts a multiboot kernel in 32-bit protected mode. There is no
 * way to ask for anything else, so every 64-bit kernel begins with a stub like
 * this one: a short stretch of 32-bit code whose only job is to build enough
 * of a 64-bit world to jump into.
 *
 * THE ORDER IS NOT NEGOTIABLE
 *
 * Long mode cannot be switched on directly. It requires paging, which requires
 * PAE, which requires page tables that already exist. So:
 *
 *   1. Build a page table covering enough memory to keep running.
 *   2. Enable PAE in CR4.
 *   3. Set the LME bit in EFER - "long mode enable". Nothing happens yet.
 *   4. Enable paging in CR0. THIS is the moment long mode activates.
 *   5. Load a 64-bit GDT and far-jump to a code segment with the L bit set.
 *
 * Between 4 and 5 the CPU is in a curious halfway state called compatibility
 * mode: long mode is on, but the current code segment is still a 32-bit one, so
 * it keeps executing 32-bit instructions. The far jump is what finishes the
 * transition, and it is the reason the jump exists at all.
 *
 * Get the order wrong and there is no error - the machine triple-faults and
 * reboots, which is why this file is worth reading slowly.
 */

.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* Page tables must be 4 KB aligned; the low 12 bits of their addresses carry
 * flags, so a misaligned table would have its address mangled. */
.section .bss
.align 4096
pml4:          .skip 4096
pdpt:          .skip 4096
pd:            .skip 4096
.align 16
stack_bottom:  .skip 16384
.global stack_top
stack_top:

.section .rodata
/* A 64-bit GDT, far smaller than the 32-bit one - long mode very nearly
 * abandons segmentation. Base and limit are ignored for code and data, and the
 * CPU behaves as though every segment spans all of memory. What remains are the
 * flag bits: present, privilege level, and L, meaning "64-bit code segment".
 *
 * Segmentation was the 1980s answer to memory protection. Paging replaced it,
 * and by 64-bit AMD simply stopped pretending otherwise.
 */
.align 8
.global gdt64
gdt64:
	.quad 0x0000000000000000        /* null descriptor               */
	.quad 0x00AF9A000000FFFF        /* kernel code: L=1, exec, DPL 0 */
	.quad 0x00CF92000000FFFF        /* kernel data                   */
gdt64_end:

gdt64_pointer:
	.word gdt64_end - gdt64 - 1
	.quad gdt64

.section .text
.code32
.global _start
.type _start, @function

_start:
	mov $stack_top, %esp

	/* GRUB left the magic number in eax and the info structure in ebx. Stash
	 * them where the 64-bit calling convention expects arguments - edi and
	 * esi - before the table building clobbers everything else. Writing a
	 * 32-bit register zeroes the upper half of its 64-bit form, so these
	 * survive the mode change intact. */
	mov %eax, %edi
	mov %ebx, %esi

	/* --- four levels, of which we fill three ---
	 *
	 * pml4[0] -> pdpt, pdpt[0] -> pd, then 512 pd entries each mapping 2 MB
	 * directly. That is the huge-page bit: a PD entry can either point at a
	 * page table or BE a 2 MB mapping by itself. One table instead of 512,
	 * and a gigabyte of identity mapping in about ten instructions. */
	mov $pdpt, %eax
	or $0x03, %eax                  /* present | writable */
	mov %eax, pml4

	mov $pd, %eax
	or $0x03, %eax
	mov %eax, pdpt

	xor %ecx, %ecx
1:
	mov %ecx, %eax
	shl $21, %eax                   /* index * 2 MB */
	or $0x83, %eax                  /* present | writable | huge */
	mov %eax, pd(, %ecx, 8)
	movl $0, pd+4(, %ecx, 8)        /* upper half of the 64-bit entry */
	inc %ecx
	cmp $512, %ecx
	jb 1b

	/* --- PAE --- */
	mov %cr4, %eax
	or $(1 << 5), %eax
	mov %eax, %cr4

	/* --- EFER: long mode enable, and no-execute enable ---
	 * EFER is a model-specific register, reached through rdmsr/wrmsr. Setting
	 * LME switches nothing on by itself; it means "long mode when paging
	 * starts". */
	mov $0xC0000080, %ecx
	rdmsr
	or $(1 << 8), %eax              /* LME */
	or $(1 << 11), %eax             /* NXE - bit 63 means no-execute */
	wrmsr

	mov $pml4, %eax
	mov %eax, %cr3

	/* --- paging, and with it long mode --- */
	mov %cr0, %eax
	or $(1 << 31), %eax             /* PG */
	or $(1 << 16), %eax             /* WP - ring 0 obeys read-only pages */
	mov %eax, %cr0

	/* Long mode is active, but this code segment is still 32-bit, so the CPU
	 * is in compatibility mode. The far jump loads a segment with the L bit
	 * set, and that finishes the job. */
	lgdt gdt64_pointer
	ljmp $0x08, $long_mode_start

.code64
long_mode_start:
	/* Segment registers are essentially decorative in long mode, but they
	 * must hold something valid. */
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs
	mov %ax, %ss

	mov $stack_top, %rsp
	xor %rbp, %rbp

	/* rdi and rsi still hold what GRUB gave us. */
	call kernel_main

	cli
2:	hlt
	jmp 2b

.section .note.GNU-stack,"",@progbits
