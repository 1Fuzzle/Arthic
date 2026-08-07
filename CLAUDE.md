# Arthic — project context

## What this is

A 32-bit x86 kernel written from scratch. Boots via GRUB/Multiboot, runs in
QEMU. Currently it initialises a VGA text terminal and prints a banner. There
is no scheduler, no memory manager, no interrupt handling, and no drivers yet.

## Who I am and how I want to work

I'm learning C through this project. I have not written C before. I read the
code you write and learn from it — so:

- **Comment heavily, and explain the C, not just the kernel.** When you use a
  pointer, a bit shift, a cast, or anything idiomatic, say what it means and
  why it's done that way. Assume I don't know it yet.
- **Explain what the hardware is doing.** The interesting part is why the code
  has to be shaped this way, not just that it works.
- **Don't skip ahead.** Small, complete, working steps beat large ones.
- **Tell me when something is a real design decision** versus just convention,
  so I know which parts I could have done differently.
- Don't flatter the code or the questions. If an idea is bad, say so and say
  why.

## Build and run

```
./build.sh          # compile only
./build.sh run      # compile, then boot in QEMU
```

Requires `base-devel` and `qemu-system-x86` on Arch. The build must stay
warning-clean — in kernel code, warnings are usually real bugs.

## Files

```
boot/boot.s          multiboot header, stack, calls kernel_main (only assembly besides interrupts.s)
kernel/main.c        kernel_main — subsystem init only, keep it small
kernel/gdt.c         GDT, flat model, ring 0 + ring 3 descriptors, no TSS
kernel/idt.c         IDT, PIC remap to 32-47, exception and IRQ dispatch
kernel/interrupts.s  48 stubs + idt_flush
kernel/shell.c       line buffer, command dispatch
kernel/tss.c         TSS - kernel stack for ring 3 entry, I/O bitmap denies port access
kernel/syscall.c     int 0x80 dispatch, user pointer validation
kernel/usermode.c    ring 3 entry/exit, demo user program in .usertext
kernel/task.c        kernel threads, round-robin scheduler, reaping
kernel/switch.s      context switch - callee-saved registers plus the stack pointer
drivers/terminal.c   VGA text, scrolling, hardware cursor, kprintf
drivers/keyboard.c   PS/2 scancode set 1, Shift + Caps Lock
drivers/timer.c      PIT tick counter
mm/pmm.c             physical frame allocator, bitmap, driven by multiboot mmap
mm/paging.c          page directory/tables, all RAM identity-mapped, CR0.WP, page fault handler
mm/kheap.c           kmalloc/kfree, first-fit, splitting, coalescing, magic guards
lib/string.c         kmemset, kmemcpy, kstrcmp, kstartswith
include/             all headers
```

New code goes in `kernel/` if it is OS machinery, `drivers/` if it talks to
specific hardware, `mm/` for memory management, `lib/` for functions that would
have come from libc. Headers always in `include/`. `main.c` stays small — if it
grows, the new thing wants its own file.

## Constraints to respect

- **Freestanding.** No libc. No `stdio.h`, no `malloc`, no `string.h`. Only
  `stdint.h` and `stddef.h` (types only, no code). If we need a function, we
  write it.
- **32-bit protected mode.** Don't introduce 64-bit assumptions.
- **No PIE.** The build uses `-no-pie` and `-fno-pie` deliberately.
- The `/DISCARD/` block in `linker.ld` exists to stop `.note` sections being
  placed ahead of the Multiboot header. Don't remove it.

## Roadmap

1. ~~Scrolling~~ done (v0.2)
2. ~~Hardware cursor via VGA I/O ports~~ done (v0.3)
3. ~~Our own `printf`~~ done (v0.4) — `%d %u %x %s %c`, no width specifiers yet
4. ~~GDT~~ done (v0.5) — no TSS
5. ~~IDT, PIC remap, timer~~ (v0.6), ~~keyboard driver + shell~~ (v0.7)
6. ~~Physical memory manager~~ (v0.9)
7. ~~Paging~~ (v1.0)
8. ~~Heap (kmalloc)~~ (v1.1)
9. TSS + user mode — ring 3 and a syscall gate  ← **next**
10. Long mode (brings a real NX bit)
7. Later: TSS + user mode, 64-bit long mode (requires paging first)

## Reference

- OSDev wiki: https://wiki.osdev.org/
- Operating Systems: Three Easy Pieces: https://pages.cs.wisc.edu/~remzi/OSTEP/
