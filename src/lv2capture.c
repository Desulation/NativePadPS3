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

void lv2capture_poll(lv2capture_t *st, uint32_t now_ms)
{
	uint8_t now[DS45_CAPTURE_MAX_LEN];
	int len = (int)g_cfg.capture_len;

	if (st->stopped || len <= 0 || len > DS45_CAPTURE_MAX_LEN)
		return;

	if (ds45_lv2_read_bytes(DS45_LV2_BASE + g_cfg.capture_off, now, len) != 0) {
		ds45_boot_mark("cap_read_failed");
		st->stopped = 1;
		return;
	}

	if (st->have && memcmp(now, st->snap, (size_t)len) == 0)
		return;

	memcpy(st->snap, now, (size_t)len);
	st->have = 1;
	ds45_boot_mark_hex("cap_t", now_ms);
	log_bytes(now, len);

	/* Every change is a file append per line: bound the trace. */
	if (++st->logs >= DS45_CAPTURE_MAX_LOGS) {
		ds45_boot_mark("cap_limit");
		st->stopped = 1;
	}
}

#endif /* DS45_LV2DUMP */
