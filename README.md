# Arthic

A 32-bit x86 kernel, built from nothing. It boots, takes control of the
machine, handles interrupts, reads the keyboard, and gives you a shell.

## Current state - v1.3

- Boots via GRUB/Multiboot into 32-bit protected mode
- VGA text terminal with scrolling and a hardware cursor
- `kprintf` with `%d %u %x %s %c %%`
- Own GDT (flat model, ring 0 and ring 3 descriptors, no TSS)
- Own IDT: 32 CPU exception handlers, 16 IRQs, PIC remapped to vectors 32-47
- PIT timer driving a tick counter
- PS/2 keyboard driver with Shift and Caps Lock
- Physical memory manager: bitmap frame allocator driven by the BIOS memory map
- Paging: all RAM identity-mapped, kernel code and rodata read-only, CR0.WP
  set, all kernel pages supervisor-only, page fault handler reporting CR2
- Kernel heap: 1 MB, first-fit with block splitting and coalescing, magic-number
  guards against double free and corruption
- TSS and ring 3: a user program runs at reduced privilege and reaches the
  kernel only through `int 0x80`, the single IDT gate with DPL 3
- Syscalls with argument validation: every pointer from ring 3 is range-checked
  and length-bounded before the kernel touches it
- Recoverable page faults, so the kernel survives a bad pointer instead of
  halting - the same mechanism Linux uses for `copy_from_user`
- Preemptive round-robin scheduler: kernel threads with their own stacks,
  switched by the timer, with finished threads reaped and their stacks returned
- A shell: `help`, `about`, `ticks`, `mem`, `alloc`, `heap`, `heaptest`,
  `tasks`, `spawn`, `user`, `wptest`, `echo`, `clear`

No filesystem yet.

### On W^X — a known gap

Plain 32-bit paging has **no no-execute bit**. Any readable page is executable.
Real W^X needs PAE or 64-bit long mode, where the entry format is wide enough
to carry an NX bit. What is enforced today is the write half: kernel code
cannot be modified, and CR0.WP makes that apply to ring 0 too. Completing W^X
is a reason to move to long mode.

## Building and running

On Arch:

```
sudo pacman -S base-devel qemu-system-x86 qemu-ui-gtk
```

Then:

```
chmod +x build.sh
./build.sh run
```

`build.sh` on its own builds without launching QEMU.

The `chmod` is needed any time you download a fresh `build.sh` — the executable
bit is a filesystem permission, not part of the file's contents, so it does not
survive a download.

## Layout

```
Arthic/
├── build.sh          compile everything, optionally boot it
├── linker.ld         memory layout
├── README.md
├── CLAUDE.md         project context for Claude Code
├── boot/
│   └── boot.s        multiboot header, stack, calls kernel_main
├── kernel/
│   ├── main.c        kernel_main — brings subsystems up, hands off to shell
│   ├── gdt.c         segment descriptors
│   ├── idt.c         interrupt table, PIC remap, dispatchers
│   ├── interrupts.s  48 assembly stubs, one per exception and IRQ
│   └── shell.c       line buffer and command dispatch
├── drivers/
│   ├── terminal.c    VGA text output, scrolling, cursor, kprintf
│   ├── keyboard.c    PS/2 scancodes to characters
│   └── timer.c       PIT tick counter
├── mm/
│   ├── pmm.c         physical frame allocator (bitmap)
│   ├── paging.c      page directory and tables, MMU setup, page fault handler
│   └── kheap.c       kmalloc / kfree — variable-sized allocation
├── lib/
│   └── string.c      kmemset, kmemcpy, kstrcmp — no libc exists
├── include/          every header
└── build/            object files (generated, git-ignored)
```

The split is by role, not by file size. `kernel/` is the machinery that makes
this an operating system; `drivers/` is code that talks to a specific piece of
hardware; `include/` is the interfaces between them. When you add something,
the question "which of those three is it?" usually answers itself, and if it
does not, that is a hint the thing is doing two jobs.

`build/` holds object files so they are not scattered through the source tree.
`./build.sh clean` deletes it.

## Build flags that matter

- `-m32` — 32-bit. The CPU is in 32-bit mode after GRUB hands over.
- `-ffreestanding -nostdlib -fno-builtin` — no standard library exists here.
- `-mno-mmx -mno-sse -mno-sse2 -mno-80387` — **required.** Without these, gcc
  optimises loops into SSE instructions, which the CPU refuses to execute
  before SSE is enabled in CR4. The result is a triple fault and a reboot loop.
- `-no-pie -fno-pie` — no position-independent code.
- `-Wall -Wextra` — the build must stay warning-clean. In kernel code,
  warnings are usually real bugs.
- `-fno-stack-protector` — required only because `__stack_chk_fail` does not
  exist yet. Worth writing later rather than simply dropping the flag.

## Debugging

When it reboots in a loop, it is triple-faulting. This shows what the CPU
actually objected to:

```
qemu-system-i386 -no-reboot -kernel arthic.bin -display none -d int
```

`-no-reboot` halts instead of resetting. `-d int` logs every interrupt and
exception with its vector, error code and instruction pointer.

To check the compiler has not emitted instructions the CPU cannot run yet:

```
objdump -d arthic.bin | grep -c xmm
```

Should be `0`.

## Roadmap

1. ~~Scrolling~~ (v0.2)
2. ~~Hardware cursor~~ (v0.3)
3. ~~`kprintf`~~ (v0.4)
4. ~~GDT~~ (v0.5)
5. ~~IDT, PIC remap, timer~~ (v0.6) and ~~keyboard, shell~~ (v0.7)
6. ~~Physical memory manager~~ (v0.9)
7. ~~Paging~~ (v1.0) — write protection done; NX blocked until PAE or long mode
8. ~~Heap (kmalloc)~~ (v1.1)
9. ~~TSS and user mode~~ (v1.2) - ring 3 and a validated syscall gate
10. ~~Scheduler~~ (v1.3) - preemptive round robin over kernel threads
11. **Next:** sleeping and blocking, so threads stop busy-waiting
12. Later: 64-bit long mode, and with it a real NX bit
13. Later: a filesystem

## Reference

- OSDev wiki — <https://wiki.osdev.org/>
- Operating Systems: Three Easy Pieces — <https://pages.cs.wisc.edu/~remzi/OSTEP/>
