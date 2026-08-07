# Arthic

A 32-bit x86 kernel, built from nothing. Currently it boots, takes control of
the machine, and draws to the screen. That is all it does, and that is already
more than most people ever see happen.

## Files

| File | What it is |
|---|---|
| `boot.s` | The first code that runs. Assembly, because C can't set up its own stack. ~40 real lines. |
| `kernel.c` | The kernel itself. Where all the C learning lives. |
| `linker.ld` | Tells the linker exactly where each piece goes in memory. |
| `build.sh` | Compiles and optionally boots it in QEMU. |

## Building it

On Arch you need:

```
sudo pacman -S base-devel qemu-system-x86
```

Then:

```
./build.sh run
```

A QEMU window opens and Arthic boots into it. Close the window to stop.

If you only want to build without running, drop the `run`.

### If the build complains

The most likely failure is 32-bit support. If `gcc -m32` errors, install
`lib32-glibc` (or on other distros, the multilib package). Everything else
should be satisfied by `base-devel`.

## Read it in this order

1. `boot.s` — short, and it explains what "no operating system underneath you"
   actually means in practice.
2. `kernel.c` from the top — types, then the screen, then the terminal
   functions, then `kernel_main` at the bottom.
3. `linker.ld` last. It's the least important to understand right now.

The comments in `kernel.c` are the lesson. Read them as prose, not as
footnotes.

## The C ideas in here worth stopping on

- **Pointers.** `uint16_t *terminal_buffer` holds an address, not a value. This
  is the single idea the rest of C hangs off.
- **Arrays are pointer arithmetic.** `buffer[i]` means "go to buffer, step i
  elements forward". There is no bounds checking. None. Ever.
- **Strings are just bytes ending in zero.** `terminal_write` walks forward
  until it finds a `\0`. That's not a simplification for teaching — that is
  genuinely all a C string is.
- **Bit shifting to pack fields.** `fg | (bg << 4)` squeezes two 4-bit values
  into one byte because that's the shape the hardware wants.
- **`static` at file scope means "private to this file."** Different meaning
  from `static` inside a function. C reuses the keyword; it's a known wart.

## Good next steps, roughly in order of difficulty

1. **Scrolling.** Right now the screen wraps to the top when it fills. Make it
   shift every row up by one instead. Pure memory copying — a good first
   exercise and it needs no new concepts.
2. **A hardware cursor.** Talk to the VGA controller over I/O ports so the
   blinking cursor follows the text. Your first real device driver.
3. **`printf`.** Write your own. Start with just `%d` and `%s`. This means
   writing integer-to-string conversion by hand, since nothing does it for you.
4. **The GDT.** Set up your own segment descriptors instead of using GRUB's.
   Dry, but it's the gateway to everything after it.
5. **Interrupts (the IDT).** The big one. Once interrupts work you can handle
   the keyboard, and once you have a keyboard you have a shell, and once you
   have a shell Arthic starts feeling like a real system.
6. **A memory manager.** Track which physical pages are free, then enable
   paging. This is where OSes get genuinely hard and genuinely interesting.

Steps 1–3 need only what's already in this repo. Step 5 is where you'll want
the OSDev wiki open alongside.

## Reference

- OSDev wiki — <https://wiki.osdev.org/> (the "Bare Bones" and "Meaty Skeleton"
  pages are the direct continuation of this code)
- Operating Systems: Three Easy Pieces — <https://pages.cs.wisc.edu/~remzi/OSTEP/>
  (free, and the best explanation of the concepts)
