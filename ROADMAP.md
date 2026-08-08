# Arthic roadmap

Two branches exist and both are real:

- **`main`** - 32-bit, v2.4. Complete: memory manager, PAE paging with NX,
  heap, ring 3, validated syscalls, preemptive scheduler with sleeping,
  mutexes with wait queues, pipes, ATA driver, block-mapped filesystem, ELF
  loader, processes with separate address spaces.
- **`long-mode`** - 64-bit, stage 2. Boots into long mode, four-level paging,
  memory manager, heap, W^X. Everything above ring 0 is still missing.

Sizes below are rough: **S** an evening, **M** a session or two, **L** several
sessions, **XL** a project in its own right.

---

## Phase 1 - bring 64-bit to parity (items 1-6)

Until this is done, `main` is the capable branch and `long-mode` is a
demonstration. Each step here is a port, so the design work is already done and
the interest is in what 64-bit changes.

**1. TSS and ring 3 (M)**
The 64-bit TSS holds no task state at all - AMD dropped hardware task
switching - only stack pointers. It also gains the IST, seven known-good stacks
the CPU can switch to for specific vectors, which is how a real kernel survives
a fault caused by a broken stack pointer.

**2. `syscall`/`sysret` (M)**
`int 0x80` still works but 64-bit has a purpose-built instruction pair that
skips the IDT entirely. Faster, and genuinely different: `syscall` does not
switch stacks for you, so the kernel must do it by hand via `swapgs` - which is
its own trap, since getting it wrong either corrupts a user pointer or leaks a
kernel one.

**3. Scheduler and context switch (M)**
Fifteen registers to save instead of eight, and the switch has to swap CR3 as
before. Mechanically similar to what exists.

**4. Locks and pipes (S)**
Almost a straight copy. `xchg` works identically.

**5. Disk driver and filesystem (M)**
Also close to a copy. The ATA driver is port I/O and does not care about mode.

**6. ELF64 loader and processes (L)**
ELF64 headers are wider but the same shape. Per-process address spaces mean a
PML4 per process rather than a PDPT - one more level to copy, one fewer thing
to think about, since the kernel's half can be shared at the top level.

---

## Phase 2 - security hardening (items 7-11)

Your stated priority, and the phase where Arthic stops merely working and starts
being defensible.

**7. Stack canaries (S)**
`-fno-stack-protector` is in the build flags only because `__stack_chk_fail`
does not exist. Write it - about ten lines - and drop the flag. A random value
between locals and the return address, checked on the way out. Catches the
classic stack smash before it can redirect execution.

**8. ASLR (M)**
Load programs at a random address rather than always `0x20000000`. Requires
position-independent executables, which is much easier in 64-bit because
RIP-relative addressing makes them nearly free. Turns "jump to a known address"
into "guess correctly out of thousands of possibilities".

**9. SMEP and SMAP (S)**
Two bits in CR4. SMEP stops the kernel executing userspace memory; SMAP stops it
*reading* userspace memory except in bracketed sections. Both close entire
vulnerability classes and cost almost nothing.

**10. A guard page under every stack (S)**
One unmapped page below each kernel stack, so a stack overflow faults
immediately rather than quietly corrupting whatever is beneath it. Currently a
runaway recursion in the kernel would eat the next task's data in silence.

**11. Syscall argument auditing (M)**
Every syscall re-examined with the three-check rule the pipe write uses. This is
the surface that matters most and it deserves a deliberate pass rather than
per-call vigilance.

---

## Phase 3 - real system behaviour (items 12-16)

**12. Interrupt-driven disk I/O (S)**
The driver currently polls and blocks the CPU for the whole transfer. Now that
`task_block` exists, sleep the caller and wake it on IRQ 14.

**13. A buffer cache (M)**
Keep recently read blocks in memory. Every filesystem operation currently goes
to the disk, including reading the same directory sector repeatedly. This is
where a filesystem stops being slow.

**14. Subdirectories (M)**
The directory is one flat table of 64 entries. Making a directory just another
file whose contents are directory entries is the standard trick, and it removes
both limits at once.

**15. Crash consistency (L)**
Right now a power cut midway through a write can leave a directory entry
pointing at unwritten data. Ordering helps; a journal is the real answer.

**16. `fork` and `exec` (L)**
The Unix process model. `fork` needs copy-on-write to be sane, which needs the
page fault handler to do real work rather than report and die - that is the
piece that makes the memory manager feel finished.

---

## Phase 4 - breadth (items 17-20)

**17. Serial output (S)**
A COM port driver, so kernel output can go to a file on the host instead of only
to a VGA screen you have to screenshot. This would have saved hours already and
is the single highest value-per-line item on this list.

**18. APIC, HPET, and the TSC (M)**
The PIC and PIT are 1981 hardware kept alive by emulation. The APIC is what
actual systems use, supports far more interrupt lines, and is a prerequisite for
using more than one CPU core.

**19. SMP (XL)**
More than one core. Everything assuming a single CPU breaks: disabling
interrupts stops *this* core only, so every interrupts-off section becomes a
real spinlock. This is the change that would force the most rethinking, and it
is worth doing for exactly that reason.

**20. Networking (XL)**
An e1000 driver, then ARP, IP, UDP, and eventually TCP. The largest item here
and the one furthest from everything else - but "Arthic replied to a ping" is a
good day.

---

## Not on this list, deliberately

**A window system, sound, USB.** Each is a large project that teaches less per
hour than the items above, and USB in particular is a swamp.

**Making Arthic a daily driver.** It will not be. The kernel is the learning
project; a system people could use is the distro path - a Linux base with your
userland on top. That is a different piece of work, and this one qualifies you
for it rather than replacing it.

---

## Suggested order

If security stays the priority: **1, 2, 7, 9, 10, 3, 6, 8**. That gets ring 3
back on the 64-bit branch, then takes the cheap hardening wins before the
expensive ones.

If you want the system to feel more real sooner: **17 first** - serial output
makes everything after it easier to debug - then 12, 13, 14.

If you want the hardest interesting thing: **16**, then **19**.
