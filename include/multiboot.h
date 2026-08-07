/* multiboot.h — the structure GRUB hands us.
 *
 * When a multiboot loader starts a kernel it leaves a pointer in ebx to a
 * block of information about the machine. The single most valuable thing in it
 * is the MEMORY MAP: a list from the BIOS saying which physical address ranges
 * actually contain usable RAM.
 *
 * You cannot guess this. Physical memory is not one continuous run — there are
 * holes for memory-mapped hardware, regions the BIOS reserves for itself, and
 * areas belonging to devices. Writing to the wrong one does not fail loudly, it
 * corrupts something and the machine misbehaves later. So we ask.
 */
#ifndef ARTHIC_MULTIBOOT_H
#define ARTHIC_MULTIBOOT_H

#include <stdint.h>

/* eax holds this on entry if we were genuinely loaded by a multiboot loader.
 * Checking it costs one comparison and rules out a whole class of confusion. */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

/* Bit 6 of `flags` set means the mmap fields below are valid. The structure is
 * old and mostly optional, so every field comes with a flag saying whether the
 * loader bothered to fill it in. Trusting a field without checking its flag is
 * how you read garbage. */
#define MULTIBOOT_INFO_MEM_MAP 0x00000040

struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;     /* KB below 1 MB   */
	uint32_t mem_upper;     /* KB above 1 MB   */
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length;   /* total bytes of the mmap buffer */
	uint32_t mmap_addr;     /* where that buffer is           */
	/* more fields follow; we do not need them yet */
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

/* One entry in the memory map.
 *
 * Note `size` does NOT include itself, so walking the list means advancing by
 * size + 4 rather than by sizeof(struct). That off-by-four is deliberate in the
 * spec and catches everyone once.
 *
 * addr and len are 64-bit even in a 32-bit kernel, because the same structure
 * serves both. Anything above 4 GB is unreachable for us and gets ignored.
 */
struct multiboot_mmap_entry {
	uint32_t size;
	uint64_t addr;
	uint64_t len;
	uint32_t type;          /* 1 = usable RAM, anything else = do not touch */
} __attribute__((packed));

#endif
