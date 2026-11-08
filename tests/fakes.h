/* Host-test fakes. Everything here is the contract tests and the code under
 * test rely on; the implementations live in fakes.c. */
#ifndef NPP_TEST_FAKES_H
#define NPP_TEST_FAKES_H

#include <stdint.h>
#include <cell/pad.h>

/* Return the failing line from run_tests(); 0 means pass. */
#define CHECK(e) do { if (!(e)) return __LINE__; } while (0)

/* Each test file defines this; fakes.c provides main(). */
int run_tests(void);

/* Settable clock (microseconds). Zeroed by fake_reset_all(). */
extern uint64_t fake_now_us;

/* Fake LV2 kernel image served through syscall 6. Sparse: pages are
 * allocated on demand, unset bytes read as 0. Bytes written outside
 * [NPP_LV2_BASE, NPP_LV2_BOUND) are ignored. */
void fake_lv2_set(uint64_t addr, const uint8_t *bytes, int len);
void fake_lv2_reset(void);
/* Count of syscall-6 reads that were misaligned or outside the window. */
extern int fake_lv2_out_of_bounds;
/* Count of all syscall-6 reads, in or out of bounds. */
extern int fake_lv2_peek_count;

/* Syscall 7 (poke) stores one big-endian qword into the fake image.
 * fake_lv2_poke_count counts all syscall-7 calls (dropped ones included);
 * fake_lv2_poke_fail_after >= 0 lets that many pokes through and silently
 * drops every later one (so a read-back fails); -1 = never drop. Both are
 * reset by fake_reset_all(). A poke that is misaligned or outside the window
 * is dropped and also counted in fake_lv2_out_of_bounds. */
extern int fake_lv2_poke_count;
extern int fake_lv2_poke_fail_after;

/* Called after every syscall-6 read (in or out of bounds) when set; a test
 * can change the image from here to simulate a concurrent kernel write.
 * Cleared by fake_reset_all(). */
extern void (*fake_lv2_on_peek)(uint64_t addr);

/* Syscall dispatcher behind system_call_N. 6 = peek; 7 = poke; 8 = fake_syscall8 if
 * set, else -1 (every syscall-8 call is logged either way); anything else
 * = -1. */
uint64_t fake_syscall(int n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

/* Syscall 8 (PS3MAPI etc.). The handler, when set, supplies the result; a3
 * is often a user pointer, cast back with (void *)(uintptr_t)a3. The first
 * FAKE_SC8_LOG calls are logged. Cleared by fake_reset_all(). */
#define FAKE_SC8_LOG 16
extern uint64_t (*fake_syscall8)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
extern int      fake_sc8_calls;
extern uint64_t fake_sc8_a1[FAKE_SC8_LOG];
extern uint64_t fake_sc8_a2[FAKE_SC8_LOG];

/* Spare-syscall routing. The kshim installer borrows an otherwise-unused
 * syscall slot: it points the slot at an OPD, calls the syscall, then restores
 * the slot. The real kernel dispatcher would double-deref the slot and call
 * the pointed-to function (the LV2 allocator, then the cache-flush routine);
 * the fake does not model that indirection, so a test routes the spare syscall
 * number to its own stand-in handler instead. fake_syscall() sends syscall
 * number fake_spare_syscall (>= 0) to fake_spare_handler if both are set,
 * passing (a1, a2) and returning its result (the allocator's address; the
 * flush result is ignored). Both are cleared by fake_reset_all(). */
extern int fake_spare_syscall;
extern uint64_t (*fake_spare_handler)(uint64_t a1, uint64_t a2);

/* sys_mutex_* fakes (tests/stubs/sys/synchronization.h). trylock fails
 * while fake_mutex_busy is set. fake_mutex_errors counts a lock taken while
 * already held (a real deadlock), an unlock of an unheld mutex, and
 * operations before create. */
extern int fake_mutex_busy;
extern int fake_mutex_depth;
extern int fake_mutex_errors;
extern int fake_mutex_live;     /* created and not destroyed */

/* npp_notify() records here. */
extern int fake_toasts;
extern char fake_last_toast[128];

/* sys_tty_write() appends here (NUL-terminated; output past the buffer is
 * dropped and counted in fake_tty_overflow). Cleared by fake_reset_all(). */
extern char fake_tty[2048];
extern int  fake_tty_overflow;

/* In-memory file system behind npp_read_file() and friends. Files are
 * heap-allocated (up to 32 MiB each); writes that cannot be stored fail
 * with -1 rather than truncating. */
void        fake_file_set(const char *path, const char *text);
/* Contents (binary-safe: use fake_file_size() for the length; also NUL-
 * terminated for text). NULL if absent. Valid until the next write/reset. */
const char *fake_file_get(const char *path);
int         fake_file_size(const char *path);  /* bytes, -1 if absent */
void        fake_file_set_bytes(const char *path, const void *data, int len);
int         fake_file_deleted(const char *path); /* 1 if npp_delete_file()d */

/* Failure injection for npp_write_file()/npp_append_file(): any write to
 * fake_write_fail_path fails; once fake_append_fail_after (>= 0) appends have
 * succeeded, the next and every later append fails. -1 / NULL = never.
 * Cleared by fake_reset_all(). */
extern const char *fake_write_fail_path;
extern int         fake_append_fail_after;

int npp_write_file(const char *path, const void *data, int len);
int npp_append_file(const char *path, const void *data, int len);
int npp_delete_file(const char *path);
int npp_file_exists(const char *path);

/* ldd_register() fake counter. A registered pad gets handle 100 + n, port 0;
 * ldd_unregister() sets the handle back to -1. */
extern int fake_ldd_registers;
extern int fake_ldd_unregisters;

/* cellPad fakes: cellPadGetInfo2 returns fake_pad_info_rc and, when that is
 * CELL_PAD_OK, a copy of fake_pad_info. cellPadLddDataInsert counts and keeps
 * the last sample. */
extern CellPadInfo2 fake_pad_info;
extern int          fake_pad_info_rc;
extern int          fake_ldd_inserts;
extern CellPadData  fake_last_insert;

/* Reset every fake above to its initial state. */
void fake_reset_all(void);

#endif /* NPP_TEST_FAKES_H */
