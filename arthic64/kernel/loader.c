/* loader.c - reading an ELF64 file and running it as its own process.
 *
 * Two things distinguish this from the ring 3 demo in usermode.c: the program
 * comes from disk rather than being linked into the kernel image, and it gets
 * its own address space rather than sharing the kernel's. Both are new, and
 * both matter more than they sound.
 *
 * WHY A SEPARATE ADDRESS SPACE, NOT JUST A SEPARATE MAPPING
 *
 * usermode.c's demo ran in the kernel's own PML4, just with some pages marked
 * user-accessible. That is fine for one embedded program, but two programs
 * both wanting to live at the same virtual address - which any ELF linked the
 * ordinary way will want, since nothing tells it to pick something else -
 * cannot coexist in one page table. A process needs its OWN root table, so
 * "address 0x400000" can mean different physical memory depending on which
 * process is running, exactly the way the 32-bit branch's per-process PDPTs
 * worked, one level higher now because 64-bit paging has one more level.
 *
 * ONE MORE LEVEL TO COPY, ONE FEWER THING TO DECIDE
 *
 * The 32-bit branch copied a page DIRECTORY per process. Here it is a PML4 -
 * 512 entries instead of 4, but the principle does not change: copy the
 * kernel's own entries so the kernel is mapped identically everywhere, leave
 * everything else empty for the process to fill. The extra level is pure
 * mechanical width, not a new idea.
 */

#include "loader.h"
#include "elf.h"
#include "fs.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "string.h"
#include "terminal.h"
#include "syscall.h"
#include "tss.h"
#include "task.h"

extern void usermode_jump(uint64_t entry, uint64_t user_stack_top);
extern void usermode_return(void);

#define MAX_SEGMENTS 8
#define MAX_PROGRAMS 4
#define MAX_PROGRAM_SIZE (64 * 1024)

struct program {
	uint64_t page_dir;   /* physical address of this process's PML4 */
	uint64_t entry;

	struct {
		uint64_t vaddr;
		uint64_t phys;
		uint64_t pages;
	} segment[MAX_SEGMENTS];

	uint64_t segments;
	uint64_t stack_phys;
	char     name[32];
	int      in_use;
};

static struct program programs[MAX_PROGRAMS];

static uint64_t flags_for_segment(uint32_t elf_flags)
{
	uint64_t flags = PAGE_PRESENT | PAGE_USER;

	if (elf_flags & PF_W)
		flags |= PAGE_WRITE;

	/* The real, unconditional other half of W^X: anything the ELF header did
	 * not mark executable gets NX, no capability check needed - 64-bit page
	 * table entries have always had room for this bit. */
	if (!(elf_flags & PF_X))
		flags |= PAGE_NX;

	return flags;
}

static int load_segment(struct program *prog, const uint8_t *file,
                        uint64_t file_size, const struct elf_program_header *ph)
{
	uint64_t page_start = ph->vaddr & ~0xFFFull;
	uint64_t within     = ph->vaddr - page_start;
	uint64_t pages      = (within + ph->memsz + PAGE_SIZE - 1) / PAGE_SIZE;

	if (pages == 0 || prog->segments >= MAX_SEGMENTS)
		return 0;

	/* Every field here came off a disk and is therefore untrusted, exactly
	 * like a syscall argument. A malformed header could ask us to copy from
	 * beyond the end of the file, or to map a page over kernel memory - both
	 * checked below, the same way the 32-bit loader checks them. */
	if (ph->filesz > ph->memsz)
		return 0;
	if (ph->offset > file_size || ph->filesz > file_size - ph->offset)
		return 0;
	if (ph->vaddr < USER_LOAD_ADDR || ph->vaddr + ph->memsz > USER_STACK_TOP)
		return 0;

	uint64_t phys = pmm_alloc_frames(pages);
	if (!phys)
		return 0;

	kmemset((void *) phys, 0, pages * PAGE_SIZE);
	kmemcpy((void *)(phys + within), file + ph->offset, ph->filesz);

	uint64_t flags = flags_for_segment(ph->flags);

	for (uint64_t i = 0; i < pages; i++) {
		if (!paging_map(page_start + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags)) {
			for (uint64_t j = 0; j < pages; j++)
				pmm_free_frame(phys + j * PAGE_SIZE);
			return 0;
		}
	}

	prog->segment[prog->segments].vaddr = page_start;
	prog->segment[prog->segments].phys  = phys;
	prog->segment[prog->segments].pages = pages;
	prog->segments++;

	kprintf("  segment at 0x%lx  %lu bytes in file, %lu in memory  %s%s%s\n",
	        ph->vaddr, ph->filesz, ph->memsz,
	        (ph->flags & PF_R) ? "r" : "-",
	        (ph->flags & PF_W) ? "w" : "-",
	        (ph->flags & PF_X) ? "x" : "-");

	return 1;
}

static void unload(struct program *prog)
{
	for (uint64_t i = 0; i < prog->segments; i++)
		for (uint64_t p = 0; p < prog->segment[i].pages; p++)
			pmm_free_frame(prog->segment[i].phys + p * PAGE_SIZE);
	prog->segments = 0;

	if (prog->stack_phys) {
		uint64_t pages = USER_STACK_SIZE / PAGE_SIZE;
		for (uint64_t i = 0; i < pages; i++)
			pmm_free_frame(prog->stack_phys + i * PAGE_SIZE);
		prog->stack_phys = 0;
	}

	/* Destroying the address space frees the page tables it added in one
	 * operation, rather than a careful walk unmapping each page first - the
	 * quiet advantage of a per-process root table. There is nothing to unmap:
	 * the mappings only ever existed inside this PML4, and it is about to
	 * stop existing. */
	if (prog->page_dir) {
		paging_destroy_address_space(prog->page_dir);
		prog->page_dir = 0;
	}

	prog->in_use = 0;
}

static void program_cleanup(void *arg)
{
	struct program *prog = (struct program *) arg;
	if (prog)
		unload(prog);
}

static void program_task(void)
{
	struct task *self = task_current();
	struct program *prog = self ? (struct program *) self->arg : 0;

	if (!prog) {
		kprintf("loader: task started with no program attached\n");
		return;
	}

	syscall_set_user_range(USER_LOAD_ADDR, USER_STACK_TOP);

	/* Does not return. The program leaves ring 3 by exiting or by faulting;
	 * either way the kernel kills the task rather than unwinding back through
	 * this function - see task_terminate and the page fault handler's ring-3
	 * path. Cleanup happens in program_cleanup once the task is off the run
	 * queue, not here. */
	usermode_jump(prog->entry, USER_STACK_TOP - 16);
}

int loader_run(const char *name)
{
	struct program *prog = 0;
	for (uint32_t i = 0; i < MAX_PROGRAMS; i++) {
		if (!programs[i].in_use) {
			prog = &programs[i];
			break;
		}
	}
	if (!prog) {
		kprintf("loader: too many programs running\n");
		return 0;
	}
	kmemset(prog, 0, sizeof(*prog));

	uint32_t size = 0;
	uint8_t *file = (uint8_t *) kmalloc(MAX_PROGRAM_SIZE);
	if (!file) {
		kprintf("loader: out of heap\n");
		return 0;
	}

	if (!fs_read(name, file, MAX_PROGRAM_SIZE, &size) ||
	    size < sizeof(struct elf_header)) {
		kprintf("loader: cannot read %s\n", name);
		kfree(file);
		return 0;
	}

	const struct elf_header *elf = (const struct elf_header *) file;

	if (elf->ident[0] != ELF_MAGIC0 || elf->ident[1] != ELF_MAGIC1 ||
	    elf->ident[2] != ELF_MAGIC2 || elf->ident[3] != ELF_MAGIC3) {
		kprintf("loader: %s is not an ELF file\n", name);
		kfree(file);
		return 0;
	}

	/* ELFCLASS64 here, not ELFCLASS32 - a 32-bit ELF binary is rejected
	 * outright rather than half-loaded with the wrong header layout. */
	if (elf->ident[4] != ELFCLASS64 || elf->ident[5] != ELFDATA2LSB ||
	    elf->type != ET_EXEC || elf->machine != EM_X86_64) {
		kprintf("loader: %s is not a 64-bit x86_64 executable\n", name);
		kfree(file);
		return 0;
	}

	if (elf->phoff + (uint64_t) elf->phnum * elf->phentsize > size) {
		kprintf("loader: program header table runs past the end of the file\n");
		kfree(file);
		return 0;
	}

	/* A fresh PML4, then switch to it so the mapping calls below apply there
	 * rather than to the kernel's own tables. Safe because the kernel is
	 * mapped identically in both - the same reasoning the scheduler's CR3
	 * switch relies on. */
	prog->page_dir = paging_create_address_space();
	if (!prog->page_dir) {
		kprintf("loader: no memory for an address space\n");
		kfree(file);
		return 0;
	}

	extern uint64_t paging_kernel_directory(void);
	extern void paging_switch(uint64_t pdpt_phys);
	uint64_t saved = paging_kernel_directory();
	paging_switch(prog->page_dir);

	for (uint32_t i = 0; i < elf->phnum; i++) {
		const struct elf_program_header *ph =
			(const struct elf_program_header *)
			(file + elf->phoff + (uint64_t) i * elf->phentsize);

		if (ph->type != PT_LOAD)
			continue;

		if (!load_segment(prog, file, size, ph)) {
			kprintf("loader: bad or unloadable segment %u\n", i);
			paging_switch(saved);
			unload(prog);
			kfree(file);
			return 0;
		}
	}

	kfree(file);

	if (prog->segments == 0) {
		kprintf("loader: nothing to load\n");
		paging_switch(saved);
		unload(prog);
		return 0;
	}

	uint64_t stack_pages = USER_STACK_SIZE / PAGE_SIZE;
	prog->stack_phys = pmm_alloc_frames(stack_pages);
	if (!prog->stack_phys) {
		kprintf("loader: no memory for the stack\n");
		paging_switch(saved);
		unload(prog);
		return 0;
	}
	kmemset((void *) prog->stack_phys, 0, USER_STACK_SIZE);

	uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
	for (uint64_t i = 0; i < stack_pages; i++) {
		if (!paging_map(stack_bottom + i * PAGE_SIZE, prog->stack_phys + i * PAGE_SIZE,
		                PAGE_PRESENT | PAGE_USER | PAGE_WRITE | PAGE_NX)) {
			kprintf("loader: could not map the stack\n");
			paging_switch(saved);
			unload(prog);
			return 0;
		}
	}

	paging_switch(saved);

	prog->entry  = elf->entry;
	prog->in_use = 1;

	kprintf("loader: %s, %u bytes, entry 0x%lx, %lu segments, own PML4\n",
	        name, size, elf->entry, prog->segments);

	if (!task_create_ex("prog", program_task, prog->page_dir, prog, program_cleanup)) {
		kprintf("loader: could not create a task for it\n");
		unload(prog);
		return 0;
	}

	return 1;
}
