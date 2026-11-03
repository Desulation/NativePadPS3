/*
 * DS45PAD - debug-build LV2 capture.
 *
 * Watches a small kernel range (CAPTURE_LEN bytes at LV2 offset CAPTURE_LV2)
 * every tick through the bounds-checked reads in lv2.c, and writes each
 * change to the boot trace: one time line, then the bytes, 24 to a line.
 * Read-only. Used to see what passes through a kernel buffer at moments an
 * outside poller cannot catch, such as a Bluetooth pad reconnecting.
 *
 * Debug VSH builds only; release and game builds contain none of it.
 */

#include <string.h>
#include <cell/pad.h>   /* CellPadData, needed by ds45pad.h */

#include "ds45pad.h"

#ifdef DS45_LV2DUMP

#include "lv2.h"

#define PER_LINE 24

static void log_bytes(const uint8_t *b, int len)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[4 + 2 * PER_LINE + 1];
	int at, i, n;

	for (at = 0; at < len; at += PER_LINE) {
		line[0] = 'c';
		line[1] = hex[(at >> 4) & 0xF];
		line[2] = hex[at & 0xF];
		line[3] = ' ';
		n = 4;
		for (i = at; i < len && i < at + PER_LINE; i++) {
			line[n++] = hex[b[i] >> 4];
			line[n++] = hex[b[i] & 0xF];
		}
		line[n] = 0;
		ds45_boot_mark(line);
	}
}

/* CAPTURE_PTR mode: follow the pointer qword at CAPTURE_PTR and read
 * CAPTURE_LEN bytes at CAPTURE_LV2 into the object it names. The object comes
 * and goes (a pad being created and torn down), so a null or unreadable
 * pointer is a state to log, not a failure: it reads no bytes, and the bounded
 * reader refuses any target outside the window before peeking. */
static int poll_ptr(lv2capture_t *st, uint8_t *now, int len)
{
	uint64_t ptr;
	int bytes;

	if (ds45_lv2_read(DS45_LV2_BASE + g_cfg.capture_ptr, &ptr) != 0)
		return -1;
	bytes = ptr != 0 &&
	        ds45_lv2_read_bytes(ptr + g_cfg.capture_off, now, len) == 0;
	if (st->have && ptr == st->ptr && bytes == st->ptr_bytes &&
	    (!bytes || memcmp(now, st->snap, (size_t)len) == 0))
		return 0;
	st->ptr = ptr;
	st->ptr_bytes = bytes;
	return 1;
}

void lv2capture_poll(lv2capture_t *st, uint32_t now_ms)
{
	uint8_t now[DS45_CAPTURE_MAX_LEN];
	int len = (int)g_cfg.capture_len;
	int changed;

	if (st->stopped || len <= 0 || len > DS45_CAPTURE_MAX_LEN)
		return;

	if (g_cfg.capture_ptr) {
		changed = poll_ptr(st, now, len);
	} else {
		changed = ds45_lv2_read_bytes(DS45_LV2_BASE + g_cfg.capture_off,
		                              now, len) != 0 ? -1 :
		          !st->have || memcmp(now, st->snap, (size_t)len) != 0;
	}
	if (changed < 0) {
		ds45_boot_mark("cap_read_failed");
		st->stopped = 1;
		return;
	}
	if (!changed)
		return;

	memcpy(st->snap, now, (size_t)len);
	st->have = 1;
	ds45_boot_mark_hex("cap_t", now_ms);
	if (g_cfg.capture_ptr) {
		ds45_boot_mark_hex("cap_p", (uint32_t)st->ptr);
		if (st->ptr_bytes)
			log_bytes(now, len);
	} else {
		log_bytes(now, len);
	}

	/* Every change is a file append per line: bound the trace. */
	if (++st->logs >= DS45_CAPTURE_MAX_LOGS) {
		ds45_boot_mark("cap_limit");
		st->stopped = 1;
	}
}

#endif /* DS45_LV2DUMP */
