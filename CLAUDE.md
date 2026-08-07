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

- `gdt.c` / `gdt.h` — Global Descriptor Table. Flat model, ring 0 and ring 3
  descriptors defined. No TSS yet.
- `boot.s` — Multiboot header, stack setup, calls `kernel_main`. The only
  assembly in the project.
- `kernel.c` — the kernel. VGA text output, terminal state.
- `linker.ld` — memory layout. Multiboot header must land within the first
  8 KiB of the binary or GRUB won't recognise it.
- `build.sh` — build script. Note the freestanding flags; they matter.

## Security is a stated priority

Arthic is meant to be a secure system. This is a design goal, not an
afterthought — prefer the secure option even when it costs effort, and say so
when a shortcut would weaken it.

- **W^X.** Once paging exists, no page is ever both writable and executable.
  Set NX on data pages. Decide this at the first page table, not later.
- **Kernel pages are supervisor-only.** Ring 3 must not be able to read kernel
  memory.
- **Enable SMEP and SMAP** when we reach CR4 configuration.
- **Fault handlers must report**, not silently halt — invisible failures cannot
  be fixed.
- **Validate anything crossing a privilege boundary.** A pointer from userspace
  is hostile until proven otherwise.
- **Bounds-check anything indexed by external input.** The realistic threat to
  this kernel is our own C bugs, not missing features.
- The build must stay clean under `-Wall -Wextra`. Warnings in kernel code are
  usually real bugs.
- `-fno-stack-protector` is currently required because `__stack_chk_fail` does
  not exist yet. Worth writing later; do not simply drop the flag.

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
5. IDT and interrupt handling → keyboard driver → shell  ← **next**
6. Physical memory manager, then paging (W^X from the start)
7. Later: TSS + user mode, 64-bit long mode (requires paging first)

## Reference

- OSDev wiki: https://wiki.osdev.org/
- Operating Systems: Three Easy Pieces: https://pages.cs.wisc.edu/~remzi/OSTEP/
