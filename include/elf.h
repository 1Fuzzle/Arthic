/* elf.h - the Executable and Linkable Format.
 *
 * A flat binary is just bytes: no header, no structure, and no way to say
 * anything about itself. ELF is what you get when you let a program describe
 * its own requirements.
 *
 * The part a loader actually cares about is small. An ELF header says "this is
 * an executable, for this architecture, start at this address, and here is a
 * table of program headers". Each program header says "take this range of the
 * file, put it at this address, make it this size, with these permissions".
 * That is the whole contract.
 *
 * The three things it buys us over a flat image:
 *
 *   - PER-SEGMENT PERMISSIONS. Code becomes read-only and executable, data
 *     becomes writable and (on hardware with an NX bit) not executable. With a
 *     flat binary the loader has no idea where one ends and the other begins,
 *     so the whole image has to share one set of flags.
 *
 *   - A DECLARED ENTRY POINT. The loader stops assuming the first byte.
 *
 *   - memsz LARGER THAN filesz. A segment can ask for more memory than it
 *     occupies in the file, and the loader zeroes the difference. That is what
 *     .bss is: a program wanting a megabyte of zeroed space no longer needs a
 *     megabyte of file to hold zeroes.
 *
 * There is far more in the format - sections, symbols, relocations, dynamic
 * linking - and a loader for static executables needs none of it. Sections are
 * for the linker; segments are for the loader.
 */
#ifndef ARTHIC_ELF_H
#define ARTHIC_ELF_H

#include <stdint.h>

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS32   1
#define ELFDATA2LSB  1
#define ET_EXEC      2
#define EM_386       3

#define PT_LOAD      1   /* the only segment type we handle */

/* Permission bits in a program header. Note they are NOT the same bit values
 * as the page table uses - a loader has to translate between the two, and
 * mixing them up produces a program that runs but is not protected. */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf_header {
	uint8_t  ident[16];      /* magic, class, endianness, version */
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint32_t entry;          /* where execution starts */
	uint32_t phoff;          /* file offset of the program header table */
	uint32_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;      /* size of one program header */
	uint16_t phnum;          /* how many there are */
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} __attribute__((packed));

struct elf_program_header {
	uint32_t type;
	uint32_t offset;         /* where in the file this segment lives  */
	uint32_t vaddr;          /* where in memory it wants to be        */
	uint32_t paddr;
	uint32_t filesz;         /* how many bytes to copy                */
	uint32_t memsz;          /* how much space to provide             */
	uint32_t flags;          /* PF_R / PF_W / PF_X                    */
	uint32_t align;
} __attribute__((packed));

#endif
