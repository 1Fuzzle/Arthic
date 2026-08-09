# Arthic 64 - stage 2 of the long mode port

This is a **separate branch of the project**, not a replacement. The 32-bit
Arthic (v2.4) has a memory manager, filesystem, processes, pipes and W^X. This
one boots into 64-bit and has a shell, and nothing else yet.

Keep both. Do not delete v2.4 to make room for this.

## What works

- GRUB's 32-bit handoff, then a full transition into 64-bit long mode
- A 64-bit GDT, and an IDT with 16-byte gates
- All 32 CPU exceptions plus 16 IRQs, PIC remapped to 32-47
- Timer and PS/2 keyboard
- VGA terminal, scrolling, hardware cursor, `kprintf` with `%lx` and `%lu`
- A physical memory manager driven by the BIOS memory map
- Managed four-level paging with 4 KB pages, replacing boot.s's huge pages
- **W^X, free with the architecture**: kernel code is r-x, everything else is
  rw- with NX. No PAE bolt-on needed - 64-bit entries have always had room.
- A kernel heap, `kmalloc` and `kfree`, 16-byte aligned
- Recoverable page faults
- A preemptive round-robin scheduler: kernel threads with their own stacks,
  switched by the timer, with sleeping, blocking, and finished threads reaped
- Mutexes (xchg-based, FIFO wait queue) and pipes (bounded ring buffer,
  blocking both ways) - direct ports; neither cares about register width
- An ATA PIO disk driver and ArthicFS v2 (block-mapped files) - genuinely
  DIRECT ports, no changes at all beyond a new build.sh entry. Files survive
  a reboot on `arthic64.img`, separate from the 32-bit branch's disk image
  since the two kernels use incompatible on-disk layouts (different FS_MAGIC).
- A shell: `help`, `about`, `ticks`, `regs`, `mem`, `heap`, `heaptest`,
  `wxtest`, `user`, `tasks`, `spawn`, `kill`, `racetest`, `locktest`,
  `pipetest`, `pipestat`, `ls`, `cat`, `write`, `append`, `rm`, `df`,
  `format`, `echo`, `clear`

## What is not here yet

ELF loader and per-process address spaces. All of that exists on the 32-bit
branch and has to be brought across.

## Building

```
chmod +x build.sh
./build.sh run
```

Note it uses `qemu-system-x86_64`, not `-i386`.

## Two things that will catch you out

**QEMU's `-kernel` refuses a 64-bit ELF.** It only accepts a 32-bit multiboot
image. The instructions inside are unaffected by the container format, so
`build.sh` runs `objcopy -O elf32-i386` to rewrite the ELF headers and leave the
code alone. That works only because the kernel is linked at 1 MB, so every
address fits in 32 bits. A kernel in the upper half of the address space would
need a real bootloader and an ISO.

**`-mno-red-zone` is not optional.** The 64-bit ABI lets a function use 128
bytes below the stack pointer without reserving them. An interrupt pushes onto
that same area and silently corrupts whatever was there. Every 64-bit kernel
turns it off, and forgetting produces bugs that appear only when an interrupt
lands on exactly the wrong instruction.

## How the transition works

Long mode cannot be switched on directly. It needs paging, which needs PAE,
which needs page tables that already exist. So `boot.s` does this, in this
order, and the order is not negotiable:

1. Build a page table - one PML4, one PDPT, one PD of 512 huge pages, giving a
   gigabyte of identity mapping in about ten instructions
2. Enable PAE in CR4
3. Set LME in EFER - nothing happens yet
4. Enable paging in CR0 - **this** is when long mode activates
5. Load a 64-bit GDT and far-jump to a code segment with the L bit set

Between 4 and 5 the CPU is in compatibility mode: long mode is on, but the
current code segment is still 32-bit, so it keeps executing 32-bit
instructions. The far jump finishes the transition. Get the order wrong and
there is no error message - the machine triple-faults and reboots.

`regs` shows the hardware confirming it. LMA - long mode *active* - is a bit the
CPU sets itself, not one we wrote.

## What changed in the C

Less than you might expect. `terminal.c`, `keyboard.c` and `string.c` needed
almost nothing. The real changes:

- `kprintf` gained `%lx` / `%lu`. On 32-bit, `int` and `long` were the same size
  and the distinction did not exist. Now it does, and getting it wrong reads
  half a value or two halves of different ones. `va_arg` cannot check.
- The IDT gate grew from 8 bytes to 16, with the handler address split across
  three fields instead of two.
- `pusha` is gone - AMD removed it - so the interrupt stubs push fifteen
  registers by hand.
- Arguments arrive in registers, so handing the register block to C is
  `mov %rsp, %rdi` rather than a push.
- The stack must be 16-byte aligned at a call, and an interrupt can arrive with
  it at any alignment, so the stubs align it deliberately and restore it after.

## Why the boot tables get replaced

`boot.s` maps the first gigabyte with 2 MB huge pages - ten instructions, and it
cannot go wrong. But one huge page carries one set of permissions for all 2 MB
of it, so kernel code and kernel data inevitably share a page and no useful
protection is possible. Rebuilding with 4 KB pages costs 64 tables for 128 MB,
about 256 KB, and buys per-page permissions.

The switchover has a wrinkle the 32-bit kernel did not: paging is ALREADY ON
when `paging_init` runs - it had to be, or we would not be in long mode. So the
new tables cannot be written to arbitrary physical addresses; they are built
through the mapping that already exists, which works because `boot.s` identity
mapped the low gigabyte. Then one write to CR3 swaps the whole structure, and
the instruction after it is fetched through the new tables.

## Four levels, and the 48-bit address space

The index shifts are 39, 30, 21 and 12 - nine bits each, because 512 entries
needs nine bits. Add them: 9+9+9+9+12 = 48. That is the address space 64-bit x86
actually provides, not 2^64. The top 16 bits of a pointer must be copies of bit
47 - "canonical form" - and addresses that break the rule fault outright, which
is why a wild pointer in 64-bit tends to be caught rather than landing somewhere
plausible.

## Next

The TSS and ring 3, which is where the 64-bit differences get interesting again:
the TSS no longer holds a task state at all, only stack pointers, and
`syscall`/`sysret` replace `int 0x80` as the way in.
