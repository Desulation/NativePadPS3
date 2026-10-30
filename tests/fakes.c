/* Host-test fakes and the test main(). See fakes.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/pad.h>
#include <sys/synchronization.h>
#include <sys/sys_time.h>
#include <sys/tty.h>
#include "ds45pad.h"
#include "lv2.h"
#include "fakes.h"

/* Functions a test may legitimately pull in from the real sources (e.g. a
 * test of util.c itself) are weak, so the real definition wins at link time. */
#define FAKE_WEAK __attribute__((weak))

uint64_t fake_now_us;

/* ---------------------------------------------------------------------- */
/* Fake LV2 image: one lazily allocated 4 KiB page per slot, 2048 slots.   */
/* ---------------------------------------------------------------------- */

#define LV2_PAGE_SHIFT 12
#define LV2_PAGE_SIZE  (1u << LV2_PAGE_SHIFT)
#define LV2_PAGES      ((unsigned)((DS45_LV2_BOUND - DS45_LV2_BASE) >> LV2_PAGE_SHIFT))

static uint8_t *lv2_pages[LV2_PAGES];
int fake_lv2_out_of_bounds;
int fake_lv2_peek_count;

void fake_lv2_reset(void)
{
	unsigned i;
	for (i = 0; i < LV2_PAGES; i++) {
		free(lv2_pages[i]);
		lv2_pages[i] = NULL;
	}
	fake_lv2_out_of_bounds = 0;
	fake_lv2_peek_count = 0;
}

void fake_lv2_set(uint64_t addr, const uint8_t *bytes, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		uint64_t a = addr + (uint64_t)i;
		unsigned page;
		if (a < DS45_LV2_BASE || a >= DS45_LV2_BOUND)
			continue;
		page = (unsigned)((a - DS45_LV2_BASE) >> LV2_PAGE_SHIFT);
		if (!lv2_pages[page]) {
			lv2_pages[page] = calloc(1, LV2_PAGE_SIZE);
			if (!lv2_pages[page]) {
				fputs("fakes: out of memory\n", stderr);
				exit(2);
			}
		}
		lv2_pages[page][(a - DS45_LV2_BASE) & (LV2_PAGE_SIZE - 1)] = bytes[i];
	}
}

void (*fake_lv2_on_peek)(uint64_t addr);

static uint64_t lv2_peek_raw(uint64_t addr)
{
	const uint8_t *pg;
	uint64_t off, v = 0;
	int i;

	fake_lv2_peek_count++;
	if ((addr & 7) || addr < DS45_LV2_BASE || addr > DS45_LV2_BOUND - 8) {
		fake_lv2_out_of_bounds++;
		return 0;
	}
	off = addr - DS45_LV2_BASE;
	pg = lv2_pages[off >> LV2_PAGE_SHIFT];
	if (!pg)
		return 0;
	pg += off & (LV2_PAGE_SIZE - 1);
	for (i = 0; i < 8; i++)           /* big-endian, like the PPU */
		v = (v << 8) | pg[i];
	return v;
}

static uint64_t lv2_peek(uint64_t addr)
{
	uint64_t v = lv2_peek_raw(addr);
	if (fake_lv2_on_peek)
		fake_lv2_on_peek(addr);
	return v;
}

uint64_t (*fake_syscall8)(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int      fake_sc8_calls;
uint64_t fake_sc8_a1[FAKE_SC8_LOG];
uint64_t fake_sc8_a2[FAKE_SC8_LOG];

uint64_t fake_syscall(int n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	switch (n) {
	case 6:
		return lv2_peek(a1);
	case 8:
		if (fake_sc8_calls < FAKE_SC8_LOG) {
			fake_sc8_a1[fake_sc8_calls] = a1;
			fake_sc8_a2[fake_sc8_calls] = a2;
		}
		fake_sc8_calls++;
		return fake_syscall8 ? fake_syscall8(a1, a2, a3, a4) : (uint64_t)-1;
	default:
		return (uint64_t)-1;
	}
}

/* ---------------------------------------------------------------------- */
/* sys_mutex_*, system clock                                                */
/* ---------------------------------------------------------------------- */

#define FAKE_EBUSY 0x8001000A

int fake_mutex_busy;
int fake_mutex_depth;
int fake_mutex_errors;
int fake_mutex_live;

int sys_mutex_create(sys_mutex_t *mutex_id, sys_mutex_attribute_t *attr)
{
	(void)attr;
	if (!mutex_id)
		return -1;
	*mutex_id = 1;
	fake_mutex_live++;
	return CELL_OK;
}

int sys_mutex_destroy(sys_mutex_t mutex_id)
{
	(void)mutex_id;
	if (fake_mutex_live <= 0 || fake_mutex_depth)
		fake_mutex_errors++;
	fake_mutex_live--;
	return CELL_OK;
}

int sys_mutex_lock(sys_mutex_t mutex_id, usecond_t timeout)
{
	(void)mutex_id; (void)timeout;
	if (fake_mutex_live <= 0 || fake_mutex_depth)
		fake_mutex_errors++;      /* non-recursive: a second lock deadlocks */
	fake_mutex_depth++;
	return CELL_OK;
}

int sys_mutex_trylock(sys_mutex_t mutex_id)
{
	(void)mutex_id;
	if (fake_mutex_live <= 0)
		fake_mutex_errors++;
	if (fake_mutex_busy || fake_mutex_depth)
		return FAKE_EBUSY;
	fake_mutex_depth++;
	return CELL_OK;
}

int sys_mutex_unlock(sys_mutex_t mutex_id)
{
	(void)mutex_id;
	if (fake_mutex_depth <= 0) {
		fake_mutex_errors++;
		return -1;
	}
	fake_mutex_depth--;
	return CELL_OK;
}

system_time_t sys_time_get_system_time(void)
{
	return (system_time_t)fake_now_us;
}

/* ---------------------------------------------------------------------- */
/* Notifications, boot marks                                                */
/* ---------------------------------------------------------------------- */

int fake_toasts;
char fake_last_toast[128];

FAKE_WEAK void ds45_notify(const char *msg)
{
	fake_toasts++;
	strncpy(fake_last_toast, msg, sizeof(fake_last_toast) - 1);
	fake_last_toast[sizeof(fake_last_toast) - 1] = '\0';
}

char fake_tty[2048];
int  fake_tty_overflow;

FAKE_WEAK int sys_tty_write(unsigned int ch, const void *buf, unsigned int len,
                            unsigned int *pwritelen)
{
	size_t have = strlen(fake_tty);

	(void)ch;
	if (have + len >= sizeof(fake_tty)) {
		fake_tty_overflow++;
		return -1;
	}
	memcpy(fake_tty + have, buf, len);
	fake_tty[have + len] = '\0';
	if (pwritelen)
		*pwritelen = len;
	return 0;
}

FAKE_WEAK void ds45_boot_mark(const char *stage) { (void)stage; }
FAKE_WEAK void ds45_boot_mark_hex(const char *stage, uint32_t value)
{
	(void)stage; (void)value;
}

/* ---------------------------------------------------------------------- */
/* In-memory files                                                          */
/* ---------------------------------------------------------------------- */

#define FAKE_FILES      32
#define FAKE_PATH_MAX   128
#define FAKE_FILE_MAX   (32 * 1024 * 1024)  /* loud failure above this */

static struct {
	char path[FAKE_PATH_MAX];
	char *data;       /* heap, len bytes + a trailing NUL */
	int  len;
	int  present;
	int  deleted;
} files[FAKE_FILES];

static int file_slot(const char *path, int create)
{
	int i, free_slot = -1;
	for (i = 0; i < FAKE_FILES; i++) {
		if (files[i].path[0] && strcmp(files[i].path, path) == 0)
			return i;
		if (!files[i].path[0] && free_slot < 0)
			free_slot = i;
	}
	if (!create || free_slot < 0 || strlen(path) >= FAKE_PATH_MAX)
		return -1;
	strcpy(files[free_slot].path, path);
	return free_slot;
}

/* Returns 0, or -1 (nothing stored) if len is invalid or too large. */
static int file_store(int i, const void *data, int len)
{
	char *p;
	if (len < 0 || len > FAKE_FILE_MAX)
		return -1;
	p = malloc((size_t)len + 1);
	if (!p)
		return -1;
	if (len)
		memcpy(p, data, (size_t)len);
	p[len] = 0;
	free(files[i].data);
	files[i].data = p;
	files[i].len = len;
	files[i].present = 1;
	files[i].deleted = 0;
	return 0;
}

void fake_file_set(const char *path, const char *text)
{
	int i = file_slot(path, 1);
	if (i < 0) {
		fputs("fakes: file table full\n", stderr);
		exit(2);
	}
	if (file_store(i, text, (int)strlen(text)) < 0) {
		fputs("fakes: fake_file_set: file too large\n", stderr);
		exit(2);
	}
}

/* Binary-safe variant for tests that need embedded zeros. */
void fake_file_set_bytes(const char *path, const void *data, int len)
{
	int i = file_slot(path, 1);
	if (i < 0 || file_store(i, data, len) < 0) {
		fputs("fakes: fake_file_set_bytes failed\n", stderr);
		exit(2);
	}
}

/* Size in bytes, or -1 if the file is absent. */
int fake_file_size(const char *path)
{
	int i = file_slot(path, 0);
	return (i >= 0 && files[i].present) ? files[i].len : -1;
}

const char *fake_file_get(const char *path)
{
	int i = file_slot(path, 0);
	return (i >= 0 && files[i].present) ? files[i].data : NULL;
}

int fake_file_deleted(const char *path)
{
	int i = file_slot(path, 0);
	return i >= 0 && files[i].deleted;
}

/* Same contract as util.c: returns bytes read (NUL-terminated within cap),
 * or -1 if the file does not exist. */
FAKE_WEAK int ds45_read_file(const char *path, char *out, int cap)
{
	int i = file_slot(path, 0), n;
	if (i < 0 || !files[i].present || cap <= 0)
		return -1;
	n = files[i].len;
	if (n > cap - 1)
		n = cap - 1;
	memcpy(out, files[i].data, (size_t)n);
	out[n] = '\0';
	return n;
}

const char *fake_write_fail_path;
int         fake_append_fail_after = -1;

FAKE_WEAK int ds45_write_file(const char *path, const void *data, int len)
{
	int i;
	if (fake_write_fail_path && strcmp(path, fake_write_fail_path) == 0)
		return -1;
	i = file_slot(path, 1);
	if (i < 0)
		return -1;
	return file_store(i, data, len);
}

/* Append (creating if absent). The fake_append_fail_after-th append and every
 * one after it fails without storing anything. */
FAKE_WEAK int ds45_append_file(const char *path, const void *data, int len)
{
	int i;
	char *p;

	if (fake_write_fail_path && strcmp(path, fake_write_fail_path) == 0)
		return -1;
	if (fake_append_fail_after == 0)
		return -1;
	if (fake_append_fail_after > 0)
		fake_append_fail_after--;
	if (len < 0 || (len > 0 && !data))
		return -1;
	i = file_slot(path, 1);
	if (i < 0)
		return -1;
	if (!files[i].present)
		return file_store(i, data, len);
	if (len > FAKE_FILE_MAX - files[i].len)
		return -1;
	p = realloc(files[i].data, (size_t)files[i].len + (size_t)len + 1);
	if (!p)
		return -1;
	if (len)
		memcpy(p + files[i].len, data, (size_t)len);
	files[i].len += len;
	p[files[i].len] = 0;
	files[i].data = p;
	return 0;
}

FAKE_WEAK int ds45_delete_file(const char *path)
{
	int i = file_slot(path, 1);
	if (i < 0)
		return -1;
	files[i].present = 0;
	files[i].deleted = 1;
	files[i].len = 0;
	free(files[i].data);
	files[i].data = NULL;
	return 0;
}

FAKE_WEAK int ds45_file_exists(const char *path)
{
	int i = file_slot(path, 0);
	return i >= 0 && files[i].present;
}

/* Only so config.c can be linked without util.c's remap tables. */
FAKE_WEAK ds45_map_t ds45_parse_map(const char *name)
{
	(void)name;
	return DS45_MAP_NONE;
}

/* ---------------------------------------------------------------------- */
/* LDD                                                                   */
/* ---------------------------------------------------------------------- */

int fake_ldd_registers;
int fake_ldd_unregisters;

FAKE_WEAK int ldd_register(ds45_pad_t *pad)
{
	fake_ldd_registers++;
	if (pad) {
		pad->handle = 100 + fake_ldd_registers;
		pad->port = 0;
	}
	return 0;
}

FAKE_WEAK void ldd_unregister(ds45_pad_t *pad)
{
	fake_ldd_unregisters++;
	if (pad)
		pad->handle = -1;
}

FAKE_WEAK int ldd_set_insert_into_game(ds45_pad_t *pad, int on)
{
	(void)pad; (void)on;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* cellPad                                                                  */
/* ---------------------------------------------------------------------- */

CellPadInfo2 fake_pad_info;
int          fake_pad_info_rc;
int          fake_ldd_inserts;
CellPadData  fake_last_insert;

FAKE_WEAK int cellPadGetInfo2(CellPadInfo2 *info)
{
	if (fake_pad_info_rc != CELL_PAD_OK)
		return fake_pad_info_rc;
	if (info)
		*info = fake_pad_info;
	return CELL_PAD_OK;
}

FAKE_WEAK int cellPadSetPortSetting(uint32_t port_no, uint32_t port_setting)
{
	(void)port_no; (void)port_setting;
	return CELL_PAD_OK;
}

FAKE_WEAK int cellPadLddDataInsert(int32_t handle, CellPadData *data)
{
	(void)handle;
	fake_ldd_inserts++;
	if (data)
		fake_last_insert = *data;
	return CELL_PAD_OK;
}

/* ---------------------------------------------------------------------- */

void fake_reset_all(void)
{
	int i;
	fake_now_us = 0;
	fake_lv2_reset();
	fake_lv2_on_peek = NULL;
	fake_syscall8 = NULL;
	fake_sc8_calls = 0;
	memset(fake_sc8_a1, 0, sizeof(fake_sc8_a1));
	memset(fake_sc8_a2, 0, sizeof(fake_sc8_a2));
	fake_mutex_busy = 0;
	fake_mutex_depth = 0;
	fake_mutex_errors = 0;
	fake_mutex_live = 0;
	fake_toasts = 0;
	fake_last_toast[0] = '\0';
	fake_tty[0] = '\0';
	fake_tty_overflow = 0;
	for (i = 0; i < FAKE_FILES; i++) {
		free(files[i].data);
		memset(&files[i], 0, sizeof(files[i]));
	}
	fake_write_fail_path = NULL;
	fake_append_fail_after = -1;
	fake_ldd_registers = 0;
	fake_ldd_unregisters = 0;
	memset(&fake_pad_info, 0, sizeof(fake_pad_info));
	fake_pad_info_rc = CELL_PAD_OK;
	fake_ldd_inserts = 0;
	memset(&fake_last_insert, 0, sizeof(fake_last_insert));
}

int main(int argc, char **argv)
{
	char name[64] = "test";
	int line;

	if (argc > 0 && argv[0]) {         /* name = executable basename */
		const char *b = argv[0], *p;
		size_t n;
		for (p = argv[0]; *p; p++)
			if (*p == '/' || *p == 92)    /* backslash */
				b = p + 1;
		n = strlen(b);
		if (n > 4 && strcmp(b + n - 4, ".exe") == 0)
			n -= 4;
		if (n >= sizeof(name))
			n = sizeof(name) - 1;
		memcpy(name, b, n);
		name[n] = '\0';
	}

	fake_reset_all();
	line = run_tests();
	fake_lv2_reset();
	if (line) {
		printf("FAIL %s:%d\n", name, line);
		return 1;
	}
	printf("PASS %s\n", name);
	return 0;
}
