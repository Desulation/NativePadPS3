/*
 * NPP - debug-build LV2 dumper.
 *
 * Supporting a new firmware means finding where its kernel keeps the
 * Bluetooth report slot, which needs a copy of that console's own kernel
 * window: the same 8 MiB range webMAN MOD's /dump.ps3?lv2 reads. This writes
 * it to the console's own HDD through the bounds-checked reads in lv2.c. The
 * dumps are the developer's own and are never committed.
 *
 * Two dumps, taken while a Bluetooth pad is connected: A with the pad
 * untouched, B with Cross held and the left stick pushed fully left. The
 * live report is then the buffer whose bytes differ between them in exactly
 * that way. A dump taken before any pad connects holds no report at all.
 *
 * Debug VSH builds only (DUMP_LV2 = 1 in nativepadps3.txt); the file is not part
 * of release or game builds, which contain none of its code or strings.
 */

#include <string.h>
#include <cell/pad.h>   /* CellPadData, needed by npp.h */

#include "npp.h"

#ifdef NPP_LV2DUMP

#include <sys/tty.h>

#include "lv2.h"
#include "kernel.h"

#define DUMP_A_BIN "/dev_hdd0/tmp/nativepadps3_lv2.bin"
#define DUMP_A_TXT "/dev_hdd0/tmp/nativepadps3_lv2.txt"
#define DUMP_B_BIN "/dev_hdd0/tmp/nativepadps3_lv2_b.bin"
#define DUMP_B_TXT "/dev_hdd0/tmp/nativepadps3_lv2_b.txt"
#define DUMP_SIZE  ((int)(NPP_LV2_BOUND - NPP_LV2_BASE))
#define CHUNK      65536

/* Without a verified record: time to read the toast before each dump. */
#define DELAY_A_MS 20000
#define DELAY_B_MS 30000
/* With one, the pad's buttons drive the dumps: dump A this long after
 * Options is released, dump B once Cross has been held this long. */
#define RELEASE_MS 3000
#define HOLD_MS    2000

enum { PH_WAIT_PAD = 0, PH_COUNT_A, PH_COUNT_B, PH_ARM_A, PH_ARM_B, PH_DONE };

/* The 8 MiB window is written in 64 KiB pieces: never 8 MiB in RAM at once. */
typedef char chunk_divides_window[(DUMP_SIZE % CHUNK == 0) ? 1 : -1];

/* Static: the plugin thread's stack is 16 KiB, and there is no allocator. */
static uint8_t chunk[CHUNK];

/* Tiny bounded text builder; n is the length so far, always < cap. */
static int put_str(char *buf, int cap, int n, const char *s)
{
	while (*s && n < cap - 1)
		buf[n++] = *s++;
	buf[n] = 0;
	return n;
}

static int put_hex(char *buf, int cap, int n, uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	int d;

	n = put_str(buf, cap, n, "0x");
	for (d = digits - 1; d >= 0 && n < cap - 1; d--)
		buf[n++] = hex[(v >> (d * 4)) & 0xF];
	buf[n] = 0;
	return n;
}

static int put_dec(char *buf, int cap, int n, uint32_t v)
{
	char tmp[10];
	int i = 0;

	do {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i < (int)sizeof(tmp));
	while (i && n < cap - 1)
		buf[n++] = tmp[--i];
	buf[n] = 0;
	return n;
}

/* One line to the PPU TTY (ProDG Target Manager / debug console): every
 * outcome, since a toast is easy to miss and a skipped dump shows none. */
static void tty(const char *msg)
{
	char line[160];
	unsigned int written;
	int n = 0;

	n = put_str(line, sizeof(line), n, "nativepadps3: ");
	n = put_str(line, sizeof(line), n, msg);
	n = put_str(line, sizeof(line), n, "\n");
	sys_tty_write(SYS_TTYP_PPU_STDOUT, line, (unsigned int)n, &written);
}

/* Instructions for the person holding the pad: toast and TTY. */
static void say(const char *msg)
{
	npp_notify(msg);
	tty(msg);
}

static int write_description(const char *txt, const char *pad)
{
	char text[256];
	char type[24];
	uint32_t ver = 0;
	int n = 0, have_fw;

	have_fw = (kernel_fw_query(type, (int)sizeof(type), &ver) == 0);

	n = put_str(text, sizeof(text), n, "base=");
	n = put_hex(text, sizeof(text), n, NPP_LV2_BASE, 16);
	n = put_str(text, sizeof(text), n, "\nbound=");
	n = put_hex(text, sizeof(text), n, NPP_LV2_BOUND, 16);
	n = put_str(text, sizeof(text), n, "\nversion=" NPP_VERSION "\nfw_type=");
	n = put_str(text, sizeof(text), n, have_fw ? type : "unknown");
	n = put_str(text, sizeof(text), n, "\nfw_version=");
	if (have_fw)
		n = put_hex(text, sizeof(text), n, ver, 8);
	else
		n = put_str(text, sizeof(text), n, "unknown");
	n = put_str(text, sizeof(text), n, "\nkmatch=");
	n = put_dec(text, sizeof(text), n, (uint32_t)kernel_active_source());
	n = put_str(text, sizeof(text), n, "\npad=");
	n = put_str(text, sizeof(text), n, pad);
	n = put_str(text, sizeof(text), n, "\n");

	return npp_write_file(txt, text, n);
}

/* One dump: 0 written or already there, -1 failed (nothing half-written
 * left behind). An existing .bin is kept: delete it to take another. */
static int dump_one(const char *bin, const char *txt, const char *pad)
{
	char msg[120];
	int off, n;

	if (npp_file_exists(bin)) {
		n = put_str(msg, sizeof(msg), 0, "LV2 dump skipped: ");
		n = put_str(msg, sizeof(msg), n, bin);
		put_str(msg, sizeof(msg), n, " exists, delete it to dump again");
		tty(msg);
		return 0;
	}
	n = put_str(msg, sizeof(msg), 0, "LV2 dump started: ");
	put_str(msg, sizeof(msg), n, bin);
	tty(msg);
	npp_boot_mark("lv2dump");

	/* Description first, so a .bin never exists without it. */
	if (write_description(txt, pad) != 0) {
		n = put_str(msg, sizeof(msg), 0, "LV2 dump failed: cannot write ");
		put_str(msg, sizeof(msg), n, txt);
		tty(msg);
		return -1;
	}

	for (off = 0; off < DUMP_SIZE; off += CHUNK) {
		int rc = npp_lv2_read_bytes(NPP_LV2_BASE + (uint64_t)off, chunk, CHUNK);

		if (rc == 0)
			rc = off == 0 ? npp_write_file(bin, chunk, CHUNK)
			              : npp_append_file(bin, chunk, CHUNK);
		if (rc != 0) {
			/* A truncated .bin would block every later run (the
			 * exists-check above) and mislead the scanner. */
			npp_delete_file(bin);
			n = put_str(msg, sizeof(msg), 0, "LV2 dump failed at offset ");
			n = put_hex(msg, sizeof(msg), n, (uint64_t)off, 8);
			put_str(msg, sizeof(msg), n, ", partial .bin deleted");
			tty(msg);
			return -1;
		}
	}

	npp_boot_mark("lv2dump_done");
	n = put_str(msg, sizeof(msg), 0, "LV2 dump written: ");
	n = put_str(msg, sizeof(msg), n, bin);
	n = put_str(msg, sizeof(msg), n, " (");
	n = put_dec(msg, sizeof(msg), n, (uint32_t)DUMP_SIZE);
	n = put_str(msg, sizeof(msg), n, " bytes) and ");
	put_str(msg, sizeof(msg), n, txt);
	tty(msg);
	return 0;
}

/* The pad's own buttons can drive the dumps only once its report is known
 * to be the pad's: a verified record (table, cache, scan), never the legacy
 * guess. Otherwise the countdowns do. */
static int buttons_trusted(void)
{
	npp_kmatch_t s = kernel_active_source();

	return s == NPP_KMATCH_TABLE || s == NPP_KMATCH_CACHE ||
	       s == NPP_KMATCH_SCAN;
}

static void start_a(lv2dump_t *st, uint32_t now_ms)
{
	if (buttons_trusted()) {
		say("LV2 dump A: press Options, then hands off");
		st->opt_down = 0;
		st->phase = PH_ARM_A;
		return;
	}
	say("LV2 dump A in 20 s: leave the pad untouched");
	st->due_ms = now_ms + DELAY_A_MS;
	st->phase = PH_COUNT_A;
}

static void start_b(lv2dump_t *st, uint32_t now_ms)
{
	if (buttons_trusted()) {
		say("LV2 dump B: hold Cross for 2 s");
		st->held = 0;
		st->phase = PH_ARM_B;
		return;
	}
	say("LV2 dump B in 30 s: hold Cross, left stick fully left");
	st->due_ms = now_ms + DELAY_B_MS;
	st->phase = PH_COUNT_B;
}

static void fail(lv2dump_t *st)
{
	st->phase = PH_DONE;
	say("LV2 dump failed: see the TTY, release the pad");
}

static void take_b(lv2dump_t *st, const char *pad)
{
	if (dump_one(DUMP_B_BIN, DUMP_B_TXT, pad) != 0) {
		fail(st);
		return;
	}
	st->phase = PH_DONE;
	say("LV2 dumps written: release the pad");
}

void lv2dump_poll(lv2dump_t *st, uint32_t now_ms)
{
	if (st->phase == PH_DONE)
		return;

	/* Once per run: nothing to do if both dumps are already there. */
	if (!st->checked) {
		st->checked = 1;
		if (npp_file_exists(DUMP_A_BIN) && npp_file_exists(DUMP_B_BIN)) {
			tty("LV2 dumps skipped: " DUMP_A_BIN " and " DUMP_B_BIN
			    " exist, delete both to dump again");
			st->phase = PH_DONE;
			return;
		}
	}

	/* Checked every tick, countdowns included: a pad that leaves restarts
	 * the countdown when it is back, never a dump without it. */
	if (!passthru_bt_pad_present()) {
		if (st->phase != PH_WAIT_PAD) {
			st->phase = PH_WAIT_PAD;
			st->said_wait = 1;
			say("LV2 dump: pad disconnected, waiting for it");
		} else if (!st->said_wait) {
			st->said_wait = 1;
			say("LV2 dump: connect the Bluetooth pad");
		}
		return;
	}

	if (st->phase == PH_WAIT_PAD) {
		if (npp_file_exists(DUMP_A_BIN)) {
			/* Kept from an earlier run, or taken before the pad left. */
			tty("LV2 dump skipped: " DUMP_A_BIN " exists, delete it to dump again");
			start_b(st, now_ms);
			return;
		}
		start_a(st, now_ms);
		return;
	}

	if (st->phase == PH_ARM_A) {
		uint16_t d1, d2;

		/* Options pressed and released: hands off, then dump A. */
		if (!passthru_source_buttons(&d1, &d2))
			return;
		if (d1 & NPP_CTRL_START) {
			st->opt_down = 1;
		} else if (st->opt_down) {
			say("LV2 dump A in 3 s: hands off");
			st->due_ms = now_ms + RELEASE_MS;
			st->phase = PH_COUNT_A;
		}
		return;
	}

	if (st->phase == PH_ARM_B) {
		uint16_t d1, d2;

		/* Cross held for HOLD_MS: dump B while it still is. */
		if (!passthru_source_buttons(&d1, &d2) || !(d2 & NPP_CTRL_CROSS)) {
			st->held = 0;
			return;
		}
		if (!st->held) {
			st->held = 1;
			st->due_ms = now_ms + HOLD_MS;
			return;
		}
		if ((int32_t)(now_ms - st->due_ms) >= 0)
			take_b(st, "cross_held");
		return;
	}

	if ((int32_t)(now_ms - st->due_ms) < 0)
		return;

	if (st->phase == PH_COUNT_A) {
		if (dump_one(DUMP_A_BIN, DUMP_A_TXT, "untouched") != 0)
			fail(st);
		else
			start_b(st, now_ms);
		return;
	}

	/* PH_COUNT_B */
	take_b(st, "cross_held_left_stick_left");
}

#endif /* NPP_LV2DUMP */
