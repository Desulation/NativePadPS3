/*
 * DS45PAD - kernel table matching and the validated cache. See kernel.h.
 */

#include <stddef.h>

#include "lv2.h"
#include "kernel.h"

/* Fingerprinted entries are added here (and wrapped in DS45_NO_TABLE, a
 * test-only define). The legacy entry has no fingerprint and is always
 * present: 0x34FBB0 - 0x3680. */
const ds45_kernel_t ds45_kernel_table[] = {
	{ "CEX 4.93 (legacy)", 0, 0, 0x800000000034C530ULL, 0xE8 },
};
const int ds45_kernel_table_count =
	(int)(sizeof(ds45_kernel_table) / sizeof(ds45_kernel_table[0]));

int kernel_fingerprint_matches(uint64_t fp_addr, uint64_t fp_hash)
{
	uint8_t buf[DS45_FP_LEN];

	if (fp_addr == 0)
		return 0;
	if (ds45_lv2_read_bytes(fp_addr, buf, DS45_FP_LEN) != 0)
		return 0;
	return ds45_fnv1a64(buf, DS45_FP_LEN) == fp_hash;
}

const ds45_kernel_t *kernel_match_table(const ds45_kernel_t *tab, int n)
{
	int i;

	if (!tab)
		return NULL;
	for (i = 0; i < n; i++)
		if (tab[i].fp_addr != 0 &&
		    kernel_fingerprint_matches(tab[i].fp_addr, tab[i].fp_hash))
			return &tab[i];
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* cache file                                                                */
/* ------------------------------------------------------------------------ */

#define CACHE_MAGIC "DS45K1"

/* Appends "0x" + lowercase hex without leading zeros ("0x0" for 0). */
static char *put_hex(char *p, uint64_t v)
{
	static const char digits[] = "0123456789abcdef";
	int s = 60;

	*p++ = '0';
	*p++ = 'x';
	while (s > 0 && ((v >> s) & 15) == 0)
		s -= 4;
	for (; s >= 0; s -= 4)
		*p++ = digits[(v >> s) & 15];
	return p;
}

/* Parses " 0x<1..16 lowercase hex digits>" at *pp: exactly one space, then
 * the value; advances *pp past it. Nothing else is accepted. */
static int get_hex(const char **pp, const char *end, uint64_t *out)
{
	const char *p = *pp;
	uint64_t v = 0;
	int n = 0;

	if (p >= end || *p != ' ')
		return -1;
	p++;
	if (end - p < 2 || p[0] != '0' || p[1] != 'x')
		return -1;
	p += 2;
	for (; p < end; p++, n++) {
		char c = *p;
		int d;

		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'f')
			d = c - 'a' + 10;
		else
			break;
		if (n >= 16)
			return -1;
		v = (v << 4) | (uint64_t)d;
	}
	if (n == 0)
		return -1;
	*out = v;
	*pp = p;
	return 0;
}

int kernel_cache_load(ds45_kernel_t *out)
{
	char buf[160];
	const char *p, *end;
	uint64_t fp_addr, fp_hash, slot, off;
	int n, i;

	if (!out)
		return -1;
	/* The real ds45_read_file does not NUL-terminate. */
	n = ds45_read_file(DS45_KERNEL_CACHE_FILE, buf, (int)sizeof(buf) - 1);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = buf;
	end = buf + n;

	for (i = 0; CACHE_MAGIC[i]; i++, p++)
		if (p >= end || *p != CACHE_MAGIC[i])
			return -1;
	if (get_hex(&p, end, &fp_addr) || get_hex(&p, end, &fp_hash) ||
	    get_hex(&p, end, &slot) || get_hex(&p, end, &off))
		return -1;
	/* The line must end here: one newline, then end of file. A missing
	 * newline is rejected so that a line truncated inside its last field
	 * cannot parse as a shorter, valid value. */
	if (p >= end || *p != '\n')
		return -1;
	p++;
	if (p != end)
		return -1;

	if (fp_addr == 0 || off > 0xFFFFFFFFULL)
		return -1;
	if (!kernel_fingerprint_matches(fp_addr, fp_hash))
		return -1;

	out->name       = "cached";
	out->fp_addr    = fp_addr;
	out->fp_hash    = fp_hash;
	out->slot_addr  = slot;
	out->report_off = (uint32_t)off;
	return 0;
}

int kernel_cache_store(const ds45_kernel_t *k)
{
	char buf[128];
	char *p = buf;
	const char *m;

	if (!k || k->fp_addr == 0)
		return -1;

	for (m = CACHE_MAGIC; *m; m++)
		*p++ = *m;
	*p++ = ' '; p = put_hex(p, k->fp_addr);
	*p++ = ' '; p = put_hex(p, k->fp_hash);
	*p++ = ' '; p = put_hex(p, k->slot_addr);
	*p++ = ' '; p = put_hex(p, k->report_off);
	*p++ = '\n';
	return ds45_write_file(DS45_KERNEL_CACHE_FILE, buf, (int)(p - buf));
}

void kernel_cache_delete(void)
{
	ds45_delete_file(DS45_KERNEL_CACHE_FILE);
}
