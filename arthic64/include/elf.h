/* elf.h - ELF64, the format a 64-bit executable actually describes itself in.
 *
 * Same idea as the 32-bit branch's elf.h - a header, and a table of program
 * headers each saying "map this range here, with these permissions" - but
 * every field that held an address or a size is now 64 bits, because that is
 * what a 64-bit program's addresses actually are.
 */
#ifndef ARTHIC_ELF_H
#define ARTHIC_ELF_H

#include <stdint.h>

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS64   2      /* differs from the 32-bit branch's ELFCLASS32 */
#define ELFDATA2LSB  1
#define ET_EXEC      2
#define EM_X86_64    62     /* differs from the 32-bit branch's EM_386 */

#define PT_LOAD      1

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* e_ident is still 16 bytes and still holds the same magic and class byte -
 * the difference is everything AFTER it. e_entry, e_phoff and friends are
 * now 64-bit fields; the on-disk offsets of every field after e_type/e_machine
 * shift accordingly, which is why this cannot share a struct with the 32-bit
 * branch's version despite looking similar. */
struct elf_header {
	uint8_t  ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} __attribute__((packed));

/* The field ORDER changes between ELF32 and ELF64 program headers, not just
 * the widths - `flags` moves from the end to right after `type` here, because
 * putting a 32-bit field there keeps the 64-bit fields that follow naturally
 * aligned. Copying the 32-bit struct and widening types in place would put
 * every field at the wrong offset; this has to be its own layout. */
struct elf_program_header {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
} __attribute__((packed));

#endif
