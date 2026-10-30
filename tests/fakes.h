/* Host-test fakes. Everything here is the contract tests and the code under
 * test rely on; the implementations live in fakes.c. */
#ifndef DS45_TEST_FAKES_H
#define DS45_TEST_FAKES_H

#include <stdint.h>

/* Return the failing line from run_tests(); 0 means pass. */
#define CHECK(e) do { if (!(e)) return __LINE__; } while (0)

/* Each test file defines this; fakes.c provides main(). */
int run_tests(void);

/* Settable clock (microseconds). Zeroed by fake_reset_all(). */
extern uint64_t fake_now_us;

/* Fake LV2 kernel image served through syscall 6. Sparse: pages are
 * allocated on demand, unset bytes read as 0. Bytes written outside
 * [DS45_LV2_BASE, DS45_LV2_BOUND) are ignored. */
void fake_lv2_set(uint64_t addr, const uint8_t *bytes, int len);
void fake_lv2_reset(void);
/* Count of syscall-6 reads that were misaligned or outside the window. */
extern int fake_lv2_out_of_bounds;
/* Count of all syscall-6 reads, in or out of bounds. */
extern int fake_lv2_peek_count;

/* Syscall dispatcher behind system_call_N. 6 = peek, anything else = -1. */
uint64_t fake_syscall(int n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

/* ds45_notify() records here. */
extern int fake_toasts;
extern char fake_last_toast[128];

/* In-memory file system behind ds45_read_file() and friends. Files are
 * heap-allocated (up to 32 MiB each); writes that cannot be stored fail
 * with -1 rather than truncating. */
void        fake_file_set(const char *path, const char *text);
/* Contents (binary-safe: use fake_file_size() for the length; also NUL-
 * terminated for text). NULL if absent. Valid until the next write/reset. */
const char *fake_file_get(const char *path);
int         fake_file_size(const char *path);  /* bytes, -1 if absent */
void        fake_file_set_bytes(const char *path, const void *data, int len);
int         fake_file_deleted(const char *path); /* 1 if ds45_delete_file()d */

int ds45_write_file(const char *path, const void *data, int len);
int ds45_delete_file(const char *path);
int ds45_file_exists(const char *path);

/* ldd_register() fake counter. */
extern int fake_ldd_registers;

/* Reset every fake above to its initial state. */
void fake_reset_all(void);

#endif /* DS45_TEST_FAKES_H */
