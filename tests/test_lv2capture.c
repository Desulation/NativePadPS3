/* SOURCES: lv2.c kernel.c lv2dump.c lv2capture.c config.c paths.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fakes.h"
#include "ds45pad.h"
#include "lv2.h"

/* Debug LV2 capture: watch a configured kernel range and trace every change.
 * Synthetic image only. */

#define OFF   0x4326C8u
#define ADDR  (DS45_LV2_BASE + OFF)

/* lv2dump.c also holds the dumper, which needs these; this test never runs it. */
int passthru_bt_pad_present(void) { return 0; }
int passthru_source_buttons(uint16_t *d1, uint16_t *d2) { (void)d1; (void)d2; return 0; }

/* Record the boot trace (overrides the weak fakes). */
static char lines[64][64];
static int  nlines;

void ds45_boot_mark(const char *stage)
{
	if (nlines < 64) {
		strncpy(lines[nlines], stage, 63);
		lines[nlines][63] = 0;
	}
	nlines++;
}

void ds45_boot_mark_hex(const char *stage, uint32_t value)
{
	char l[64];

	snprintf(l, sizeof(l), "%s=0x%08X", stage, (unsigned)value);
	ds45_boot_mark(l);
}

static lv2capture_t cap;

static void setup(uint32_t off, uint32_t len)
{
	uint8_t img[72];
	int i;

	fake_reset_all();
	nlines = 0;
	memset(&cap, 0, sizeof(cap));
	for (i = 0; i < 72; i++)
		img[i] = (uint8_t)(0xA1 + i);
	fake_lv2_set(ADDR, img, 72);
	g_cfg.capture_ptr = 0;
	g_cfg.capture_off = off;
	g_cfg.capture_len = len;
}

static int test_off_by_default(void)
{
	ds45_config_t c;

	ds45_config_defaults(&c);
	if (c.capture_len != 0) return __LINE__;
	setup(OFF, 0);
	lv2capture_poll(&cap, 10);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(nlines == 0);
	return 0;
}

/* First read is logged in full: a time line, then 24 bytes per line. */
static int test_first_read_logged(void)
{
	setup(OFF, 72);
	lv2capture_poll(&cap, 10);
	CHECK(nlines == 4);
	CHECK(strcmp(lines[0], "cap_t=0x0000000A") == 0);
	CHECK(strcmp(lines[1], "c00 A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8") == 0);
	CHECK(strncmp(lines[2], "c18 B9BA", 8) == 0);
	CHECK(strncmp(lines[3], "c30 D1D2", 8) == 0);
	CHECK(strlen(lines[3]) == 4 + 48);
	return 0;
}

/* Unchanged bytes are not logged again; a change is, with its own time. */
static int test_only_changes_logged(void)
{
	uint8_t b = 0x02;

	setup(OFF, 72);
	lv2capture_poll(&cap, 10);
	lv2capture_poll(&cap, 20);
	lv2capture_poll(&cap, 30);
	CHECK(nlines == 4);
	fake_lv2_set(ADDR, &b, 1);
	lv2capture_poll(&cap, 40);
	CHECK(nlines == 8);
	CHECK(strcmp(lines[4], "cap_t=0x00000028") == 0);
	CHECK(strncmp(lines[5], "c00 02A2", 8) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A length that is not a whole number of lines still prints every byte. */
static int test_short_last_line(void)
{
	setup(OFF, 32);
	lv2capture_poll(&cap, 1);
	CHECK(nlines == 3);
	CHECK(strlen(lines[1]) == 4 + 48);
	CHECK(strcmp(lines[2], "c18 B9BABBBCBDBEBFC0") == 0);
	return 0;
}

/* A range the bounded reader refuses: one line, then silence, no peeks. */
static int test_read_failure_once(void)
{
	setup(0x7FFFF8u, 16);                       /* runs past the window */
	lv2capture_poll(&cap, 1);
	lv2capture_poll(&cap, 2);
	lv2capture_poll(&cap, 3);
	CHECK(nlines == 1);
	CHECK(strcmp(lines[0], "cap_read_failed") == 0);
	CHECK(fake_lv2_peek_count == 0);
	return 0;
}

/* The trace is bounded: after the limit one marker, then no more reads. */
static int test_log_limit(void)
{
	uint8_t b;
	int i, peeks;

	setup(OFF, 8);
	for (i = 0; i < DS45_CAPTURE_MAX_LOGS + 5; i++) {
		b = (uint8_t)i;
		fake_lv2_set(ADDR, &b, 1);
		lv2capture_poll(&cap, (uint32_t)i);
	}
	CHECK(nlines == 2 * DS45_CAPTURE_MAX_LOGS + 1);
	CHECK(cap.logs == DS45_CAPTURE_MAX_LOGS);
	CHECK(cap.stopped);
	peeks = fake_lv2_peek_count;
	lv2capture_poll(&cap, 99999);
	CHECK(fake_lv2_peek_count == peeks);
	CHECK(nlines == 2 * DS45_CAPTURE_MAX_LOGS + 1);
	return 0;
}

/* CAPTURE_LV2 / CAPTURE_LEN: debug-only keys, validated. */
static int test_config_keys(void)
{
	ds45_config_t c;

	fake_reset_all();
	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_LV2 = 0x4326C8\nCAPTURE_LEN = 72\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_off == 0x4326C8u);
	CHECK(c.capture_len == 72);

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_LV2 = 0x4326C9\nCAPTURE_LEN = 70\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_off == 0);                  /* not 8-aligned */
	CHECK(c.capture_len == 0);                  /* not a multiple of 8 */

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_LEN = 136\nCAPTURE_LV2 = 0x900000\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_len == 0);                  /* above the 128-byte maximum */
	CHECK(c.capture_off == 0);                  /* outside the 8 MiB window */

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_LV2 = 0x7FFFF8\nCAPTURE_LEN = 128\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_off == 0x7FFFF8u);          /* last doubleword */
	CHECK(c.capture_len == 128);                /* the maximum */

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_LV2 = -8\nCAPTURE_LEN = -8\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_off == 0);
	CHECK(c.capture_len == 0);
	return 0;
}

/* CAPTURE_PTR: watch through a pointer. The qword at CAPTURE_PTR holds an
 * object address; CAPTURE_LV2 is then the offset into that object. */

#define PTR_OFF  0x51B268u
#define PTR_ADDR (DS45_LV2_BASE + PTR_OFF)
#define OBJ_OFF  0x5C7800u
#define OBJ2_OFF 0x5D0000u

static void set_ptr(uint64_t value)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(value >> (56 - 8 * i));
	fake_lv2_set(PTR_ADDR, b, 8);
}

static void setup_ptr(uint32_t field_off, uint32_t len)
{
	uint8_t img[0x100];
	int i;

	fake_reset_all();
	nlines = 0;
	memset(&cap, 0, sizeof(cap));
	for (i = 0; i < 0x100; i++)
		img[i] = (uint8_t)i;
	fake_lv2_set(DS45_LV2_BASE + OBJ_OFF, img, 0x100);
	for (i = 0; i < 0x100; i++)
		img[i] = (uint8_t)(0xFF - i);
	fake_lv2_set(DS45_LV2_BASE + OBJ2_OFF, img, 0x100);
	g_cfg.capture_ptr = PTR_OFF;
	g_cfg.capture_off = field_off;
	g_cfg.capture_len = len;
}

/* A null pointer is logged (once) with no bytes, and the capture keeps going. */
static int test_ptr_null_logged_once(void)
{
	setup_ptr(0x80, 8);
	set_ptr(0);
	lv2capture_poll(&cap, 10);
	lv2capture_poll(&cap, 20);
	CHECK(nlines == 2);
	CHECK(strcmp(lines[0], "cap_t=0x0000000A") == 0);
	CHECK(strcmp(lines[1], "cap_p=0x00000000") == 0);
	CHECK(!cap.stopped);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A live pointer: its target is logged, then the bytes at target + offset. */
static int test_ptr_follows_target(void)
{
	setup_ptr(0x80, 8);
	set_ptr(DS45_LV2_BASE + OBJ_OFF);
	lv2capture_poll(&cap, 10);
	CHECK(nlines == 3);
	CHECK(strcmp(lines[1], "cap_p=0x005C7800") == 0);
	CHECK(strcmp(lines[2], "c00 8081828384858687") == 0);
	return 0;
}

/* Pointer appears, moves to another object, then goes back to null: each
 * transition is logged, with the new object's bytes. */
static int test_ptr_transitions_logged(void)
{
	setup_ptr(0x80, 8);
	set_ptr(0);
	lv2capture_poll(&cap, 1);
	set_ptr(DS45_LV2_BASE + OBJ_OFF);
	lv2capture_poll(&cap, 2);
	set_ptr(DS45_LV2_BASE + OBJ2_OFF);
	lv2capture_poll(&cap, 3);
	set_ptr(0);
	lv2capture_poll(&cap, 4);
	CHECK(nlines == 2 + 3 + 3 + 2);
	CHECK(strcmp(lines[3], "cap_p=0x005C7800") == 0);
	CHECK(strcmp(lines[6], "cap_p=0x005D0000") == 0);
	CHECK(strcmp(lines[7], "c00 7F7E7D7C7B7A7978") == 0);
	CHECK(strcmp(lines[9], "cap_p=0x00000000") == 0);
	return 0;
}

/* Same pointer, changed field: logged as a change. */
static int test_ptr_field_change_logged(void)
{
	uint8_t b = 0x00;

	setup_ptr(0x80, 8);
	set_ptr(DS45_LV2_BASE + OBJ_OFF);
	lv2capture_poll(&cap, 1);
	lv2capture_poll(&cap, 2);
	CHECK(nlines == 3);
	fake_lv2_set(DS45_LV2_BASE + OBJ_OFF + 0x80, &b, 1);
	lv2capture_poll(&cap, 3);
	CHECK(nlines == 6);
	CHECK(strcmp(lines[5], "c00 0081828384858687") == 0);
	return 0;
}

/* A pointer outside the window (or misaligned) is logged without bytes and
 * never peeked; the capture keeps going and recovers when the pointer does. */
static int test_ptr_bad_target_not_read(void)
{
	setup_ptr(0x80, 8);
	set_ptr(0x0000000012345678ULL);
	lv2capture_poll(&cap, 1);
	CHECK(nlines == 2);
	CHECK(strcmp(lines[1], "cap_p=0x12345678") == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(!cap.stopped);
	set_ptr(DS45_LV2_BASE + OBJ_OFF + 4);           /* misaligned */
	lv2capture_poll(&cap, 2);
	CHECK(nlines == 4);
	CHECK(fake_lv2_out_of_bounds == 0);
	set_ptr(DS45_LV2_BASE + OBJ_OFF);
	lv2capture_poll(&cap, 3);
	CHECK(nlines == 7);
	CHECK(!cap.stopped);
	return 0;
}

/* CAPTURE_PTR: debug-only key, 8-aligned and inside the window; off by default. */
static int test_config_ptr_key(void)
{
	ds45_config_t c;

	fake_reset_all();
	ds45_config_defaults(&c);
	CHECK(c.capture_ptr == 0);
	fake_file_set("/c.txt", "CAPTURE_PTR = 0x51B268\nCAPTURE_LV2 = 0x60\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_ptr == 0x51B268u);
	CHECK(c.capture_off == 0x60u);

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_PTR = 0x51B26C\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_ptr == 0);                  /* not 8-aligned */

	ds45_config_defaults(&c);
	fake_file_set("/c.txt", "CAPTURE_PTR = 0x800000\n");
	ds45_config_load(&c, "/c.txt");
	CHECK(c.capture_ptr == 0);                  /* outside the window */
	return 0;
}

int run_tests(void)
{
	int r;

	if ((r = test_off_by_default()) != 0) return r;
	if ((r = test_first_read_logged()) != 0) return r;
	if ((r = test_only_changes_logged()) != 0) return r;
	if ((r = test_short_last_line()) != 0) return r;
	if ((r = test_read_failure_once()) != 0) return r;
	if ((r = test_log_limit()) != 0) return r;
	if ((r = test_config_keys()) != 0) return r;
	if ((r = test_ptr_null_logged_once()) != 0) return r;
	if ((r = test_ptr_follows_target()) != 0) return r;
	if ((r = test_ptr_transitions_logged()) != 0) return r;
	if ((r = test_ptr_field_change_logged()) != 0) return r;
	if ((r = test_ptr_bad_target_not_read()) != 0) return r;
	if ((r = test_config_ptr_key()) != 0) return r;
	return 0;
}
