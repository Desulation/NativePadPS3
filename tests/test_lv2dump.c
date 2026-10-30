/* SOURCES: lv2.c kernel.c lv2dump.c config.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kernel.h"

/* Synthetic image only: made-up qwords and a made-up PS3MAPI answer. */

#define A_BIN   "/dev_hdd0/tmp/ds45pad_lv2.bin"
#define A_TXT   "/dev_hdd0/tmp/ds45pad_lv2.txt"
#define B_BIN   "/dev_hdd0/tmp/ds45pad_lv2_b.bin"
#define B_TXT   "/dev_hdd0/tmp/ds45pad_lv2_b.txt"
#define WINDOW  ((int)(DS45_LV2_BOUND - DS45_LV2_BASE))
#define CHUNK   65536

#define T_A     20000u    /* dump A is due 20 s after the pad shows up */
#define T_B     50000u    /* dump B 30 s after dump A */

static const uint8_t Q_FIRST[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
static const uint8_t Q_LAST[8]  = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18 };
static const uint8_t Q_PREV[8]  = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09 };
static const uint8_t Q_NEXT[8]  = { 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x19 };

/* passthru.c is not linked: the test decides whether a Bluetooth pad is
 * connected. */
static int pad_on;
int passthru_bt_pad_present(void) { return pad_on; }

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

static lv2dump_t st;

static void setup(void)
{
	static const ds45_kernel_t legacy[1] = {
		{ "legacy", 0, 0, DS45_LV2_BASE + 0x345670, 0xE8 }
	};

	fake_reset_all();
	memset(&st, 0, sizeof(st));
	pad_on = 1;
	fake_syscall8 = fake_mapi;
	fake_lv2_set(DS45_LV2_BASE, Q_FIRST, 8);
	fake_lv2_set(DS45_LV2_BASE + CHUNK - 8, Q_PREV, 8);   /* chunk edge */
	fake_lv2_set(DS45_LV2_BASE + CHUNK, Q_NEXT, 8);
	fake_lv2_set(DS45_LV2_BOUND - 8, Q_LAST, 8);
	/* an active kernel record, so kmatch= is not just the default 0 */
	if (kernel_resolve_from(legacy, 1, NULL, NULL) != DS45_KMATCH_LEGACY)
		fake_reset_all();      /* makes the checks below fail loudly */
}

static int bin_ok(const char *path)
{
	const char *bin = fake_file_get(path);

	return fake_file_size(path) == WINDOW && bin != NULL &&
	       memcmp(bin, Q_FIRST, 8) == 0 &&
	       memcmp(bin + WINDOW - 8, Q_LAST, 8) == 0 &&
	       memcmp(bin + CHUNK - 8, Q_PREV, 8) == 0 &&       /* chunk order */
	       memcmp(bin + CHUNK, Q_NEXT, 8) == 0;
}

static int test_waits_for_pad(void)
{
	setup();
	pad_on = 0;
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, 60000);
	lv2dump_poll(&st, 120000);

	CHECK(fake_lv2_peek_count == 0);
	CHECK(fake_file_size(A_TXT) == -1);
	CHECK(fake_toasts == 1);                             /* said once */
	CHECK(strcmp(fake_last_toast, "LV2 dump: connect the Bluetooth pad") == 0);
	CHECK(strcmp(fake_tty, "ds45pad: LV2 dump: connect the Bluetooth pad\n") == 0);
	return 0;
}

static int test_two_dumps_with_countdown(void)
{
	const char *txt;

	setup();
	lv2dump_poll(&st, 0);
	CHECK(strcmp(fake_last_toast, "LV2 dump A in 20 s: leave the pad untouched") == 0);
	lv2dump_poll(&st, T_A - 1);
	CHECK(fake_lv2_peek_count == 0);                     /* not yet */

	lv2dump_poll(&st, T_A);
	CHECK(bin_ok(A_BIN));
	CHECK(fake_file_size(B_BIN) == -1);
	CHECK(strcmp(fake_last_toast, "LV2 dump B in 30 s: hold Cross, left stick fully left") == 0);
	lv2dump_poll(&st, T_B - 1);
	CHECK(fake_file_size(B_BIN) == -1);                  /* not yet */

	lv2dump_poll(&st, T_B);
	CHECK(bin_ok(B_BIN));
	CHECK(strcmp(fake_last_toast, "LV2 dumps written: release the pad") == 0);
	CHECK(fake_toasts == 3);

	txt = fake_file_get(A_TXT);
	CHECK(txt != NULL);
	CHECK(strstr(txt, "base=0x8000000000000000\n") != NULL);
	CHECK(strstr(txt, "bound=0x8000000000800000\n") != NULL);
	CHECK(strstr(txt, "version=" DS45PAD_VERSION "\n") != NULL);
	CHECK(strstr(txt, "fw_type=DEX\n") != NULL);
	CHECK(strstr(txt, "fw_version=0x00000492\n") != NULL);
	CHECK(strstr(txt, "kmatch=4\n") != NULL);            /* DS45_KMATCH_LEGACY */
	CHECK(strstr(txt, "pad=untouched\n") != NULL);
	txt = fake_file_get(B_TXT);
	CHECK(txt != NULL);
	CHECK(strstr(txt, "pad=cross_held_left_stick_left\n") != NULL);

	/* Toasts are easy to miss; every step also goes to the TTY. */
	CHECK(strcmp(fake_tty,
	             "ds45pad: LV2 dump A in 20 s: leave the pad untouched\n"
	             "ds45pad: LV2 dump started: " A_BIN "\n"
	             "ds45pad: LV2 dump written: " A_BIN " (8388608 bytes) and " A_TXT "\n"
	             "ds45pad: LV2 dump B in 30 s: hold Cross, left stick fully left\n"
	             "ds45pad: LV2 dump started: " B_BIN "\n"
	             "ds45pad: LV2 dump written: " B_BIN " (8388608 bytes) and " B_TXT "\n"
	             "ds45pad: LV2 dumps written: release the pad\n") == 0);
	CHECK(fake_tty_overflow == 0);

	/* Done: nothing more, however long it keeps being polled. */
	lv2dump_poll(&st, T_B + 100000);
	CHECK(fake_lv2_peek_count == 2 * (WINDOW / 8));
	CHECK(fake_toasts == 3);
	return 0;
}

static int test_skips_when_both_exist(void)
{
	setup();
	fake_file_set(A_BIN, "old");
	fake_file_set(B_BIN, "old");
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_B);

	CHECK(fake_file_size(A_BIN) == 3);                   /* untouched */
	CHECK(fake_file_size(B_BIN) == 3);
	CHECK(fake_file_size(A_TXT) == -1);
	CHECK(fake_toasts == 0);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(fake_sc8_calls == 0);
	CHECK(strcmp(fake_tty,
	             "ds45pad: LV2 dumps skipped: " A_BIN " and " B_BIN
	             " exist, delete both to dump again\n") == 0);
	return 0;
}

/* A kept from an earlier run (say B failed): no A countdown, straight to
 * B's, and B still taken. */
static int test_existing_a_goes_straight_to_b(void)
{
	setup();
	fake_file_set(A_BIN, "old");
	lv2dump_poll(&st, 0);
	CHECK(strcmp(fake_last_toast, "LV2 dump B in 30 s: hold Cross, left stick fully left") == 0);
	CHECK(fake_toasts == 1);
	lv2dump_poll(&st, T_B - T_A - 1);
	CHECK(fake_file_size(B_BIN) == -1);
	lv2dump_poll(&st, T_B - T_A);

	CHECK(fake_file_size(A_BIN) == 3);
	CHECK(bin_ok(B_BIN));
	CHECK(strstr(fake_tty, "ds45pad: LV2 dump skipped: " A_BIN
	                       " exists, delete it to dump again\n") != NULL);
	CHECK(strcmp(fake_last_toast, "LV2 dumps written: release the pad") == 0);
	return 0;
}

/* The pad leaves in the middle of a countdown: said at once, no dump, and
 * the countdown starts over when it is back. */
static int test_pad_lost_restarts_countdown(void)
{
	setup();
	lv2dump_poll(&st, 0);
	pad_on = 0;
	lv2dump_poll(&st, 1000);
	CHECK(strcmp(fake_last_toast, "LV2 dump: pad disconnected, waiting for it") == 0);
	lv2dump_poll(&st, T_A);                              /* still gone: quiet */
	lv2dump_poll(&st, T_A + 1000);
	CHECK(fake_toasts == 2);
	CHECK(fake_lv2_peek_count == 0);

	pad_on = 1;
	lv2dump_poll(&st, 60000);
	CHECK(strcmp(fake_last_toast, "LV2 dump A in 20 s: leave the pad untouched") == 0);
	lv2dump_poll(&st, 60000 + T_A - 1);
	CHECK(fake_lv2_peek_count == 0);
	lv2dump_poll(&st, 60000 + T_A);
	CHECK(bin_ok(A_BIN));
	return 0;
}

/* Lost during B's countdown: A is already there, so when the pad is back
 * the B countdown restarts, not A's. */
static int test_pad_lost_during_b(void)
{
	setup();
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);
	CHECK(bin_ok(A_BIN));
	pad_on = 0;
	lv2dump_poll(&st, T_A + 2000);
	CHECK(strcmp(fake_last_toast, "LV2 dump: pad disconnected, waiting for it") == 0);

	pad_on = 1;
	lv2dump_poll(&st, 100000);
	CHECK(strcmp(fake_last_toast, "LV2 dump B in 30 s: hold Cross, left stick fully left") == 0);
	lv2dump_poll(&st, 100000 + (T_B - T_A) - 1);
	CHECK(fake_file_size(B_BIN) == -1);
	lv2dump_poll(&st, 100000 + (T_B - T_A));
	CHECK(bin_ok(B_BIN));
	CHECK(fake_lv2_peek_count == 2 * (WINDOW / 8));      /* A not dumped twice */
	return 0;
}

/* The countdown survives the uint32 millisecond counter wrapping. */
static int test_countdown_across_wrap(void)
{
	const uint32_t t0 = 0xFFFFFFFFu - 1000u;

	setup();
	lv2dump_poll(&st, t0);
	lv2dump_poll(&st, 0xFFFFFFFFu);
	lv2dump_poll(&st, (uint32_t)(t0 + T_A - 1));
	CHECK(fake_lv2_peek_count == 0);
	lv2dump_poll(&st, (uint32_t)(t0 + T_A));
	CHECK(bin_ok(A_BIN));
	return 0;
}

/* A failed dump B still tells the person holding the pad to let go. */
static int test_b_failure_says_release(void)
{
	setup();
	fake_write_fail_path = B_TXT;
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);
	lv2dump_poll(&st, T_B);

	CHECK(bin_ok(A_BIN));
	CHECK(fake_file_size(B_BIN) == -1);
	CHECK(strcmp(fake_last_toast, "LV2 dump failed: see the TTY, release the pad") == 0);
	CHECK(strstr(fake_tty, "ds45pad: LV2 dump failed: cannot write " B_TXT "\n") != NULL);
	lv2dump_poll(&st, T_B + 100000);                     /* done */
	CHECK(fake_toasts == 3);
	return 0;
}

static int test_dump_reads_stay_in_window(void)
{
	setup();
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);
	lv2dump_poll(&st, T_B);

	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(fake_lv2_peek_count == 2 * (WINDOW / 8));      /* each qword once per dump */
	return 0;
}

/* A failed chunk must not leave a truncated .bin that the skip-if-exists
 * check would keep for ever, must not announce success, and stops the run:
 * no dump B after a failed A. */
static int test_dump_failure_deletes_partial(void)
{
	setup();
	fake_append_fail_after = 5;
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);

	CHECK(fake_file_size(A_BIN) == -1);
	CHECK(fake_file_deleted(A_BIN));
	CHECK(fake_toasts == 2);                             /* countdown, failure */
	CHECK(strcmp(fake_last_toast, "LV2 dump failed: see the TTY, release the pad") == 0);
	/* 1 write + 5 appends succeeded: the 7th chunk (offset 6 * 64 KiB) failed */
	CHECK(strcmp(fake_tty,
	             "ds45pad: LV2 dump A in 20 s: leave the pad untouched\n"
	             "ds45pad: LV2 dump started: " A_BIN "\n"
	             "ds45pad: LV2 dump failed at offset 0x00060000, partial .bin deleted\n"
	             "ds45pad: LV2 dump failed: see the TTY, release the pad\n") == 0);
	/* the run is over: with writes working again, still no dump B */
	fake_append_fail_after = -1;
	{
		int peeks = fake_lv2_peek_count;

		lv2dump_poll(&st, T_B);
		lv2dump_poll(&st, T_B + 100000);
		CHECK(fake_file_size(B_BIN) == -1);
		CHECK(fake_lv2_peek_count == peeks);
	}

	/* the next run (next boot) is not blocked by the failed one */
	memset(&st, 0, sizeof(st));
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);
	CHECK(bin_ok(A_BIN));
	return 0;
}

/* No PS3MAPI (plain CFW): the dump still works, the text says so. */
static int test_dump_without_ps3mapi(void)
{
	const char *txt;

	setup();
	fake_syscall8 = NULL;
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);

	CHECK(bin_ok(A_BIN));
	txt = fake_file_get(A_TXT);
	CHECK(txt != NULL);
	CHECK(strstr(txt, "fw_type=unknown\n") != NULL);
	CHECK(strstr(txt, "fw_version=unknown\n") != NULL);
	return 0;
}

/* The metadata file goes first: a .bin without its .txt must never exist. */
static int test_dump_needs_text_file(void)
{
	setup();
	fake_write_fail_path = A_TXT;
	lv2dump_poll(&st, 0);
	lv2dump_poll(&st, T_A);

	CHECK(fake_file_size(A_BIN) == -1);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(strstr(fake_tty, "ds45pad: LV2 dump failed: cannot write " A_TXT "\n") != NULL);
	CHECK(fake_toasts == 2);                             /* countdown, failure */
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
	if ((r = test_waits_for_pad()) != 0) return r;
	if ((r = test_two_dumps_with_countdown()) != 0) return r;
	if ((r = test_skips_when_both_exist()) != 0) return r;
	if ((r = test_existing_a_goes_straight_to_b()) != 0) return r;
	if ((r = test_pad_lost_restarts_countdown()) != 0) return r;
	if ((r = test_pad_lost_during_b()) != 0) return r;
	if ((r = test_countdown_across_wrap()) != 0) return r;
	if ((r = test_b_failure_says_release()) != 0) return r;
	if ((r = test_dump_reads_stay_in_window()) != 0) return r;
	if ((r = test_dump_failure_deletes_partial()) != 0) return r;
	if ((r = test_dump_without_ps3mapi()) != 0) return r;
	if ((r = test_dump_needs_text_file()) != 0) return r;
	if ((r = test_dump_config_key()) != 0) return r;
	return 0;
}
