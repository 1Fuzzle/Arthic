# Arthic

A 32-bit x86 kernel, built from nothing. It boots, takes control of the
machine, handles interrupts, reads the keyboard, and gives you a shell.

## Current state — v0.9

- Boots via GRUB/Multiboot into 32-bit protected mode
- VGA text terminal with scrolling and a hardware cursor
- `kprintf` with `%d %u %x %s %c %%`
- Own GDT (flat model, ring 0 and ring 3 descriptors, no TSS)
- Own IDT: 32 CPU exception handlers, 16 IRQs, PIC remapped to vectors 32-47
- PIT timer driving a tick counter
- PS/2 keyboard driver with Shift and Caps Lock
- Physical memory manager: bitmap frame allocator driven by the BIOS memory map
- A shell: `help`, `about`, `ticks`, `mem`, `alloc`, `echo <text>`, `clear`

No paging, no filesystem, no user mode yet.

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
│   └── pmm.c         physical frame allocator (bitmap)
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
7. **Next:** paging — with W^X and NX from the first page table, not
   retrofitted
8. Later: a heap (kmalloc) on top of the frame allocator
9. Later: TSS and user mode, then 64-bit long mode (requires paging first)

## Reference

- OSDev wiki — <https://wiki.osdev.org/>
- Operating Systems: Three Easy Pieces — <https://pages.cs.wisc.edu/~remzi/OSTEP/>
