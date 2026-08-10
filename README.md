# Arthic

A 32-bit x86 kernel, built from nothing. It boots, takes control of the
machine, handles interrupts, reads the keyboard, and gives you a shell.

## Current state - v2.5

- Boots via GRUB/Multiboot into 32-bit protected mode
- VGA text terminal with scrolling and a hardware cursor
- `kprintf` with `%d %u %x %s %c %%`
- Own GDT (flat model, ring 0 and ring 3 descriptors, no TSS)
- Own IDT: 32 CPU exception handlers, 16 IRQs, PIC remapped to vectors 32-47
- PIT timer driving a tick counter
- PS/2 keyboard driver with Shift and Caps Lock
- Real stack canaries: `-fstack-protector-all` with a plain global guard
  (`-mstack-protector-guard=global` - x86's default reads a TLS slot this
  kernel does not have), checked on every function's return; `ssptest`
  demonstrates a caught overflow
- Physical memory manager: bitmap frame allocator driven by the BIOS memory map
- Paging with PAE: three-level tables, 64-bit entries, all RAM identity-mapped,
  kernel code and rodata read-only, CR0.WP set, kernel pages supervisor-only,
  page fault handler reporting CR2 and distinguishing instruction fetches
- **NX**: data and stack pages are non-executable. W^X is now complete.
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
- Sleeping: a task can ask to be skipped until a tick count, so waiting costs
  nothing and an idle system halts instead of spinning
- Mutexes built on `xchg`, with per-lock FIFO wait queues: a blocked thread is
  not a scheduling candidate at all until the holder wakes it
- A demonstration of the race a lock prevents, and of the lost-wakeup race that
  naive blocking would introduce
- ATA PIO disk driver: LBA28, read and write sectors, with cache flush
- ArthicFS: superblock, 32-entry directory, block bitmap, and files stored as a
  block LIST - 12 direct blocks plus one indirect block, ext2's inode design in
  miniature. Files need not be contiguous, can grow, and survive a reboot
- An ELF loader: parses program headers, maps each segment with its own
  permissions (code read-only, data writable), zeroes `.bss` rather than storing
  it, and takes the entry point from the file
- A ring 3 fault kills the program, not the kernel
- **Processes**: each loaded program gets its own page directory and its own
  task, so several can be resident at once - all mapped at 0x20000000, in
  different physical memory, none able to see the others
- Pipes: a bounded ring buffer with blocking on both sides, so a fast producer
  is made to wait rather than dropping data or growing without limit
- Pipes reachable from ring 3, so two separate processes can talk through one
- Program arguments: `run prog write` and `run prog read` are the same binary
  behaving differently
- `kill` - terminate a running task from the shell, and `spin` to give it
  something that genuinely will not stop on its own
- Console output serialised, and per-process line buffering so a whole LINE is
  atomic rather than a single write
- A shell: `help`, `about`, `ticks`, `mem`, `alloc`, `heap`, `heaptest`,
  `tasks`, `spawn`, `racetest`, `locktest`, `ls`, `cat`, `write`, `rm`, `df`,
  `format`, `install`, `run`, `spin`, `kill`, `pipetest`, `pipestat`,
  `append`, `bigfile`, `user`, `wptest`, `echo`, `clear`

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
│   ├── timer.c       PIT tick counter
│   └── ata.c         PIO disk driver
├── fs/
│   └── fs.c          ArthicFS - format, directory, block bitmap, files
├── user/
│   ├── prog.c        a program - built separately, knows nothing of the kernel
│   └── prog.ld       linked at 0x20000000, with separate text and data segments
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

`arthic.img` is a 16 MB disk image, created automatically on first run and kept
between runs so files persist. Delete it by hand to start from a blank disk.

**The on-disk format changed in v2.3.** The superblock magic changed with it, so
an older image is refused rather than misread - run `format` once after
upgrading.

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

### Done

1. ~~Scrolling~~ (v0.2)
2. ~~Hardware cursor~~ (v0.3)
3. ~~`kprintf`~~ (v0.4)
4. ~~GDT~~ (v0.5)
5. ~~IDT, PIC remap, timer~~ (v0.6) and ~~keyboard, shell~~ (v0.7)
6. ~~Physical memory manager~~ (v0.9)
7. ~~Paging~~ (v1.0)
8. ~~Heap (kmalloc)~~ (v1.1)
9. ~~TSS and user mode~~ (v1.2) - ring 3 and a validated syscall gate
10. ~~Scheduler~~ (v1.3) - preemptive round robin over kernel threads
11. ~~Sleeping~~ (v1.4)
12. ~~Mutexes~~ (v1.5)
13. ~~Wait queues~~ (v1.6)
14. ~~Filesystem~~ (v1.7)
15. ~~Loading a program from disk~~ (v1.8)
16. ~~ELF loading~~ (v1.9)
17. ~~Processes with separate address spaces~~ (v2.0)
18. ~~kill, and pipes~~ (v2.1)
19. ~~Pipes for ring 3, program arguments, console lock~~ (v2.2)
20. ~~Line buffering and block-mapped files~~ (v2.3)
21. ~~NX via PAE - W^X complete~~ (v2.4)
22. ~~64-bit long mode, brought to full parity~~ - boots, four-level paging,
    memory manager, heap, real TSS with `SYSCALL`/`SYSRET`, scheduler, locks,
    pipes, disk driver and filesystem, ELF64 loader with per-process address
    spaces, stack canaries, SMEP/SMAP, and a guard page backed by an
    IST-based double-fault handler (`long-mode` branch, stage 10)
23. ~~Stack canaries on this branch~~ - `-fstack-protector-all` with a plain
    global guard (`-mstack-protector-guard=global` - x86's default reads a
    TLS slot this kernel does not have), `ssptest` demonstrating a caught
    overflow

### Planned

Sizes are rough: **S** an evening, **M** a session or two, **L** several
sessions, **XL** a project in its own right.

**Security hardening** - the same three items just finished on `long-mode`;
this branch still needs the other two.

24. **SMEP and SMAP** (S) - two bits in CR4. One stops the kernel executing
    userspace memory, the other stops it reading userspace memory outside
    bracketed sections. Both close whole vulnerability classes.
25. **Guard page under every stack** (S) - one unmapped page below each kernel
    stack, so an overflow faults immediately instead of quietly eating the next
    task's data. Needs a working double-fault handler alongside it - see the
    `long-mode` branch's stage 10 notes for why the guard page alone is not
    enough on its own.
26. **ASLR** (M) - load programs at a random address instead of always
    `0x20000000`. Needs position-independent executables - a bigger lift here
    than on `long-mode`, which gets RIP-relative addressing nearly for free.
27. **A deliberate syscall audit** (M) - every syscall re-examined with the
    three-check rule (start in range, length sane, end in range). This is the
    surface that matters most.

**Real system behaviour**

28. **Serial output** (S) - a COM port driver so kernel output goes to a file on
    the host instead of only to a screen you have to photograph. Highest value
    per line on this list.
29. **Interrupt-driven disk I/O** (S) - the driver polls and blocks the CPU for
    the whole transfer. Sleep the caller, wake it on IRQ 14.
30. **A buffer cache** (M) - keep recently read blocks in memory. Every
    filesystem operation currently goes to the disk, including reading the same
    directory sector repeatedly.
31. **Subdirectories** (M) - make a directory just another file whose contents
    are directory entries. Removes both the 64-file limit and the flat namespace
    at once.
32. **Crash consistency** (L) - a power cut mid-write can currently leave a
    directory entry pointing at unwritten data. Ordering helps; a journal is the
    real answer.
33. **`fork` and `exec`** (L) - the Unix process model. `fork` needs
    copy-on-write, which needs the page fault handler to do real work rather
    than report and die. That is the piece that makes the memory manager feel
    finished.

**Breadth**

34. **APIC, HPET and the TSC** (M) - the PIC and PIT are 1981 hardware kept
    alive by emulation. The APIC is what real systems use and a prerequisite for
    more than one core.
35. **SMP** (XL) - more than one CPU. Everything assuming a single core breaks:
    disabling interrupts stops *this* core only, so every interrupts-off section
    becomes a real spinlock. The change that would force the most rethinking,
    and worth doing for exactly that reason.
36. **Networking** (XL) - an e1000 driver, then ARP, IP, UDP, eventually TCP.
    The largest item here and the furthest from everything else, but "Arthic
    replied to a ping" is a good day.

### Deliberately not planned

A window system, sound, and USB - each a large project that teaches less per
hour than the above, and USB in particular is a swamp.

Making Arthic a daily driver. It will not be one. The kernel is the learning
project; a system people could actually use is the distro path, a Linux base
with a custom userland on top. That is different work, and this qualifies you
for it rather than replacing it.

### A known limitation

`kill` refuses a task that is BLOCKED. A blocked task is linked into some wait
queue by a pointer that queue owns, and freeing it would leave that queue
following a dangling pointer. Doing it properly means every wait queue can have
members removed from underneath it - Linux uses a signal that wakes the task so
it can unwind and remove itself. That is a project of its own, not a missing
line.

## Reference

- OSDev wiki — <https://wiki.osdev.org/>
- Operating Systems: Three Easy Pieces — <https://pages.cs.wisc.edu/~remzi/OSTEP/>
