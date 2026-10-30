/*
 * DS45PAD - debug-build LV2 dumper.
 *
 * Supporting a new firmware means finding where its kernel keeps the
 * Bluetooth report slot, which needs a copy of that console's own kernel
 * window: the same 8 MiB range webMAN MOD's /dump.ps3?lv2 reads. This writes
 * it to the console's own HDD, once, through the bounds-checked reads in
 * lv2.c. The dumps are the developer's own and are never committed.
 *
 * Debug VSH builds only (DUMP_LV2 = 1 in ds45pad.txt); the file is not part
 * of release or game builds, which contain none of its code or strings.
 */

#include <string.h>
#include <cell/pad.h>   /* CellPadData, needed by ds45pad.h */

#include "ds45pad.h"

#ifdef DS45_LV2DUMP

#include <sys/tty.h>

#include "lv2.h"
#include "kernel.h"

#define DUMP_BIN   "/dev_hdd0/tmp/ds45pad_lv2.bin"
#define DUMP_TXT   "/dev_hdd0/tmp/ds45pad_lv2.txt"
#define DUMP_SIZE  ((int)(DS45_LV2_BOUND - DS45_LV2_BASE))
#define CHUNK      65536

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
 * outcome, since the toast is easy to miss and a skipped dump shows none. */
static void tty(const char *msg)
{
	char line[160];
	unsigned int written;
	int n = 0;

	n = put_str(line, sizeof(line), n, "ds45pad: ");
	n = put_str(line, sizeof(line), n, msg);
	n = put_str(line, sizeof(line), n, "\n");
	sys_tty_write(SYS_TTYP_PPU_STDOUT, line, (unsigned int)n, &written);
}

static int write_description(void)
{
	char text[256];
	char type[24];
	uint32_t ver = 0;
	int n = 0, have_fw;

	have_fw = (kernel_fw_query(type, (int)sizeof(type), &ver) == 0);

	n = put_str(text, sizeof(text), n, "base=");
	n = put_hex(text, sizeof(text), n, DS45_LV2_BASE, 16);
	n = put_str(text, sizeof(text), n, "\nbound=");
	n = put_hex(text, sizeof(text), n, DS45_LV2_BOUND, 16);
	n = put_str(text, sizeof(text), n, "\nversion=" DS45PAD_VERSION "\nfw_type=");
	n = put_str(text, sizeof(text), n, have_fw ? type : "unknown");
	n = put_str(text, sizeof(text), n, "\nfw_version=");
	if (have_fw)
		n = put_hex(text, sizeof(text), n, ver, 8);
	else
		n = put_str(text, sizeof(text), n, "unknown");
	n = put_str(text, sizeof(text), n, "\nkmatch=");
	n = put_dec(text, sizeof(text), n, (uint32_t)kernel_active_source());
	n = put_str(text, sizeof(text), n, "\n");

	return ds45_write_file(DUMP_TXT, text, n);
}

void lv2dump_run(void)
{
	int off;

	/* One dump per console state: delete the files to take another. */
	if (ds45_file_exists(DUMP_BIN)) {
		tty("LV2 dump skipped: " DUMP_BIN " exists, delete it to dump again");
		return;
	}
	tty("LV2 dump started");

	/* Description first, so a .bin never exists without it. */
	if (write_description() != 0) {
		tty("LV2 dump failed: cannot write " DUMP_TXT);
		return;
	}

	for (off = 0; off < DUMP_SIZE; off += CHUNK) {
		int rc = ds45_lv2_read_bytes(DS45_LV2_BASE + (uint64_t)off, chunk, CHUNK);

		if (rc == 0)
			rc = off == 0 ? ds45_write_file(DUMP_BIN, chunk, CHUNK)
			              : ds45_append_file(DUMP_BIN, chunk, CHUNK);
		if (rc != 0) {
			char msg[80];
			int n = 0;

			/* A truncated .bin would block every later run (the
			 * exists-check above) and mislead the scanner. */
			ds45_delete_file(DUMP_BIN);
			n = put_str(msg, sizeof(msg), n, "LV2 dump failed at offset ");
			n = put_hex(msg, sizeof(msg), n, (uint64_t)off, 8);
			put_str(msg, sizeof(msg), n, ", partial .bin deleted");
			tty(msg);
			return;
		}
	}

	ds45_notify("LV2 dump written");
	{
		char msg[120];
		int n = 0;

		n = put_str(msg, sizeof(msg), n, "LV2 dump written: " DUMP_BIN " (");
		n = put_dec(msg, sizeof(msg), n, (uint32_t)DUMP_SIZE);
		put_str(msg, sizeof(msg), n, " bytes) and " DUMP_TXT);
		tty(msg);
	}
}

#endif /* DS45_LV2DUMP */
