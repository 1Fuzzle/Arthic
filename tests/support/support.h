/* support.h - the fakes the tests link in place of the real kernel.
 *
 * Each of these stands in for a subsystem the module under test calls into.
 * The declarations here are the extra handles the TESTS need - a way to look
 * at what the fake recorded, or to make it misbehave on purpose. The functions
 * the kernel itself calls (kprintf, ata_read_sector, task_block, ...) are
 * declared in the kernel's own headers; the fakes simply provide different
 * definitions of them, and the linker is none the wiser.
 */
#ifndef ARTHIC_TEST_SUPPORT_H
#define ARTHIC_TEST_SUPPORT_H

#include <stdint.h>

/* ---- console -------------------------------------------------------------
 * kprintf's output is captured into a buffer instead of going to the screen,
 * so a test can assert on what a module reported - "kfree: double free" is a
 * behaviour worth checking, not noise to be discarded. */
void        console_reset(void);
const char *console_text(void);
int         console_contains(const char *needle);

/* ---- physical memory -----------------------------------------------------
 * A slab of ordinary malloc'd memory pretending to be physical frames. Enough
 * for kheap.c, which only wants one contiguous run of them. */
void     fake_pmm_reset(uint32_t frames_available);
void     fake_pmm_free_all(void);
uint32_t fake_pmm_alloc_calls(void);

/* ---- disk ----------------------------------------------------------------
 * A RAM disk behind the ata_* interface. `fake_disk_fail_writes_after` makes
 * the disk start refusing writes, which is the only way to reach fs.c's I/O
 * failure paths. */
void     fake_disk_reset(uint32_t sectors);
void     fake_disk_release(void);
void     fake_disk_fail_writes_after(uint32_t successful_writes);
void     fake_disk_fail_reads_after(uint32_t successful_reads);
uint32_t fake_disk_write_count(void);
uint32_t fake_disk_read_count(void);
uint8_t *fake_disk_sector(uint32_t lba);

/* ---- tasks and interrupts ------------------------------------------------
 * There is no scheduler here, so task_block cannot switch away - it would
 * never come back. Instead the test installs a hook that runs whenever the
 * code under test blocks, standing in for "another task got the CPU and did
 * something". That is what lets a single-threaded test exercise the paths that
 * only happen when a pipe is full or a mutex is held. */
struct task;

void         fake_task_reset(void);
struct task *fake_task_make(const char *name);
void         fake_task_set_current(struct task *t);
void         fake_task_on_block(void (*hook)(void *arg), void *arg);
uint32_t     fake_task_block_count(void);
uint32_t     fake_task_unblock_count(void);
struct task *fake_task_last_unblocked(void);
int          fake_task_interrupts_enabled(void);
uint32_t     fake_task_irq_depth(void);

#endif
