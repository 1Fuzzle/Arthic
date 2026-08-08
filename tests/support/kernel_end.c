/* kernel_end.c - the linker symbol, faked.
 *
 * linker.ld places `kernel_end` immediately after the kernel image, and pmm.c
 * takes its ADDRESS to decide where the frame bitmap goes - the value stored
 * there is meaningless. There is no linker script here, so the test provides a
 * symbol of its own with a chunk of memory behind it for the bitmap to live
 * in.
 *
 * `alias` makes kernel_end another name for `region`, which is how you give a
 * symbol a type that disagrees with its definition without lying to the
 * compiler. Page-aligning the region matters: pmm.c rounds the bitmap address
 * up to a page boundary, and if that rounding walked off the end of the array
 * the bitmap would be written into whatever followed.
 *
 * 256 KB is far more than needed. The bitmap for the 256 MB machine the tests
 * describe is 8 KB.
 */
#include <stdint.h>

static uint8_t region[256 * 1024] __attribute__((aligned(4096)));

extern uint32_t kernel_end __attribute__((alias("region")));
