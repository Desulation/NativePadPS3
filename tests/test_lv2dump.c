/* SOURCES: lv2.c kernel.c lv2dump.c config.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kernel.h"

/* Synthetic image only: made-up qwords and a made-up PS3MAPI answer. */

#define BIN_PATH "/dev_hdd0/tmp/ds45pad_lv2.bin"
#define TXT_PATH "/dev_hdd0/tmp/ds45pad_lv2.txt"
#define WINDOW   ((int)(DS45_LV2_BOUND - DS45_LV2_BASE))
#define CHUNK    65536

static const uint8_t Q_FIRST[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
static const uint8_t Q_LAST[8]  = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18 };
static const uint8_t Q_PREV[8]  = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09 };
static const uint8_t Q_NEXT[8]  = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x19 };

/* Record the boot trace (overrides the weak fake) to check kernel_log_fw()
 * still says what it did before the query was shared with the dumper. */
static char traces[4][64];
static int  trace_count;

void ds45_boot_mark(const char *stage)
{
	if (trace_count < 4) {
		strncpy(traces[trace_count], stage, 63);
		traces[trace_count][63] = 0;
	}
	trace_count++;
}

void ds45_boot_mark_hex(const char *stage, uint32_t value)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[80];
	int i = 0, d;

	while (stage[i] && i < 60) { line[i] = stage[i]; i++; }
	line[i++] = '=';
	line[i++] = '0';
	line[i++] = 'x';
	for (d = 28; d >= 0; d -= 4)
		line[i++] = hex[(value >> d) & 0xF];
	line[i] = 0;
	ds45_boot_mark(line);
}

/* PS3MAPI over syscall 8: a1 = 0x7777, a2 = opcode, a3 = out buffer. */
static uint64_t fake_mapi(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	(void)a4;
	if (a1 != 0x7777)
		return (uint64_t)-1;
	if (a2 == 0x0013) {
		memcpy((void *)(uintptr_t)a3, "DEX", 4);
		return 0;
	}
	if (a2 == 0x0014)
		return 0x0492;
	return (uint64_t)-1;
}

static void setup(void)
{
	static const ds45_kernel_t legacy[1] = {
		{ "legacy", 0, 0, DS45_LV2_BASE + 0x345670, 0xE8 }
	};

	fake_reset_all();
	fake_syscall8 = fake_mapi;
	fake_lv2_set(DS45_LV2_BASE, Q_FIRST, 8);
	fake_lv2_set(DS45_LV2_BASE + CHUNK - 8, Q_PREV, 8);   /* chunk edge */
	fake_lv2_set(DS45_LV2_BASE + CHUNK, Q_NEXT, 8);
	fake_lv2_set(DS45_LV2_BOUND - 8, Q_LAST, 8);
	/* an active kernel record, so kmatch= is not just the default 0 */
	if (kernel_resolve_from(legacy, 1, NULL, NULL) != DS45_KMATCH_LEGACY)
		fake_reset_all();      /* makes the checks below fail loudly */
}

static int test_dump_writes_both_files(void)
{
	const char *bin, *txt;

	setup();
	lv2dump_run();

	CHECK(fake_file_size(BIN_PATH) == WINDOW);
	bin = fake_file_get(BIN_PATH);
	CHECK(bin != NULL);
	CHECK(memcmp(bin, Q_FIRST, 8) == 0);
	CHECK(memcmp(bin + WINDOW - 8, Q_LAST, 8) == 0);
	CHECK(memcmp(bin + CHUNK - 8, Q_PREV, 8) == 0);      /* chunk order */
	CHECK(memcmp(bin + CHUNK, Q_NEXT, 8) == 0);

	txt = fake_file_get(TXT_PATH);
	CHECK(txt != NULL);
	CHECK(strstr(txt, "base=0x8000000000000000\n") != NULL);
	CHECK(strstr(txt, "bound=0x8000000000800000\n") != NULL);
	CHECK(strstr(txt, "version=" DS45PAD_VERSION "\n") != NULL);
	CHECK(strstr(txt, "fw_type=DEX\n") != NULL);
	CHECK(strstr(txt, "fw_version=0x00000492\n") != NULL);
	CHECK(strstr(txt, "kmatch=4\n") != NULL);            /* DS45_KMATCH_LEGACY */

	CHECK(fake_toasts == 1);
	CHECK(strcmp(fake_last_toast, "LV2 dump written") == 0);
	return 0;
}

static int test_dump_skips_when_exists(void)
{
	setup();
	fake_file_set(BIN_PATH, "old");
	lv2dump_run();

	CHECK(fake_file_size(BIN_PATH) == 3);                /* untouched */
	CHECK(fake_file_size(TXT_PATH) == -1);
	CHECK(fake_toasts == 0);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(fake_sc8_calls == 0);
	return 0;
}

static int test_dump_reads_stay_in_window(void)
{
	setup();
	lv2dump_run();

	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(fake_lv2_peek_count == WINDOW / 8);            /* each qword once */
	return 0;
}

/* A failed chunk must not leave a truncated .bin that the skip-if-exists
 * check would keep for ever, and must not announce success. */
static int test_dump_failure_deletes_partial(void)
{
	setup();
	fake_append_fail_after = 5;
	lv2dump_run();

	CHECK(fake_file_size(BIN_PATH) == -1);
	CHECK(fake_file_deleted(BIN_PATH));
	CHECK(fake_toasts == 0);

	/* the next run is not blocked by the failed one */
	fake_append_fail_after = -1;
	lv2dump_run();
	CHECK(fake_file_size(BIN_PATH) == WINDOW);
	CHECK(fake_toasts == 1);
	return 0;
}

/* No PS3MAPI (plain CFW): the dump still works, the text says so. */
static int test_dump_without_ps3mapi(void)
{
	const char *txt;

	setup();
	fake_syscall8 = NULL;
	lv2dump_run();

	CHECK(fake_file_size(BIN_PATH) == WINDOW);
	txt = fake_file_get(TXT_PATH);
	CHECK(txt != NULL);
	CHECK(strstr(txt, "fw_type=unknown\n") != NULL);
	CHECK(strstr(txt, "fw_version=unknown\n") != NULL);
	return 0;
}

/* The metadata file goes first: a .bin without its .txt must never exist. */
static int test_dump_needs_text_file(void)
{
	setup();
	fake_write_fail_path = TXT_PATH;
	lv2dump_run();

	CHECK(fake_file_size(BIN_PATH) == -1);
	CHECK(fake_toasts == 0);
	CHECK(fake_lv2_peek_count == 0);
	return 0;
}

/* DUMP_LV2 is a debug-only config key. */
static int test_dump_config_key(void)
{
	ds45_config_t c;

	fake_reset_all();
	ds45_config_defaults(&c);
	CHECK(c.dump_lv2 == 0);
	fake_file_set("/c.txt", "DUMP_LV2 = 1\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.dump_lv2 == 1);
	return 0;
}

static int test_log_fw_unchanged(void)
{
	setup();
	trace_count = 0;
	kernel_log_fw();
	CHECK(trace_count == 2);
	CHECK(strcmp(traces[0], "fw_type=DEX") == 0);
	CHECK(strcmp(traces[1], "fw_version=0x00000492") == 0);

	setup();
	fake_syscall8 = NULL;                       /* no PS3MAPI */
	trace_count = 0;
	kernel_log_fw();
	CHECK(trace_count == 2);
	CHECK(strncmp(traces[0], "fw_type_err=0x", 14) == 0);
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_log_fw_unchanged()) != 0) return r;
	if ((r = test_dump_writes_both_files()) != 0) return r;
	if ((r = test_dump_skips_when_exists()) != 0) return r;
	if ((r = test_dump_reads_stay_in_window()) != 0) return r;
	if ((r = test_dump_failure_deletes_partial()) != 0) return r;
	if ((r = test_dump_without_ps3mapi()) != 0) return r;
	if ((r = test_dump_needs_text_file()) != 0) return r;
	if ((r = test_dump_config_key()) != 0) return r;
	return 0;
}
