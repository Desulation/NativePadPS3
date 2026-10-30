/* Host-test fakes and the test main(). See fakes.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cell/pad.h>
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

static uint64_t lv2_peek(uint64_t addr)
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

uint64_t fake_syscall(int n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	(void)a2; (void)a3; (void)a4;
	switch (n) {
	case 6:  return lv2_peek(a1);
	default: return (uint64_t)-1;     /* syscall 8 etc.: error */
	}
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

FAKE_WEAK int ds45_write_file(const char *path, const void *data, int len)
{
	int i = file_slot(path, 1);
	if (i < 0)
		return -1;
	return file_store(i, data, len);
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

/* ---------------------------------------------------------------------- */
/* LDD                                                                      */
/* ---------------------------------------------------------------------- */

int fake_ldd_registers;

FAKE_WEAK int ldd_register(ds45_pad_t *pad)
{
	(void)pad;
	fake_ldd_registers++;
	return 0;
}

FAKE_WEAK void ldd_unregister(ds45_pad_t *pad) { (void)pad; }

/* ---------------------------------------------------------------------- */

void fake_reset_all(void)
{
	int i;
	fake_now_us = 0;
	fake_lv2_reset();
	fake_toasts = 0;
	fake_last_toast[0] = '\0';
	for (i = 0; i < FAKE_FILES; i++) {
		free(files[i].data);
		memset(&files[i], 0, sizeof(files[i]));
	}
	fake_ldd_registers = 0;
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
