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

/* ------------------------------------------------------------------------ */
/* signature scan                                                            */
/* ------------------------------------------------------------------------ */

/* The real pattern comes from a later task; until then the scan never runs. */
const ds45_scan_pattern_t ds45_scan_pattern = { NULL, 0, 0, 0 };

#define RING_MASK (DS45_SCAN_MAX_INSNS - 1)
typedef char ds45_scan_ring_is_pow2[(DS45_SCAN_MAX_INSNS & RING_MASK) == 0 ? 1 : -1];

typedef struct {
	uint64_t insns_addr;   /* address of insns[0] */
	uint64_t start;        /* function start (its stdu); 0 = none */
	uint64_t toc;          /* from the descriptor; 0 = none */
	int32_t  disp;
	uint32_t report_off;
} scan_hit_t;

static int insn_is(const ds45_insn_match_t *m, uint32_t w)
{
	return (w & m->mask) == m->value;
}

/* Sign-extends the low 16 bits without relying on implementation-defined
 * narrowing conversions. */
static int32_t simm16(uint32_t w)
{
	int32_t v = (int32_t)(w & 0xFFFF);
	return (v & 0x8000) ? v - 0x10000 : v;
}

static int pattern_valid(const ds45_scan_pattern_t *pat)
{
	return pat && pat->insns && pat->count > 0 &&
	       pat->count <= DS45_SCAN_MAX_INSNS &&
	       pat->toc_ld_index >= 0 && pat->toc_ld_index < pat->count &&
	       pat->report_off_index >= 0 && pat->report_off_index < pat->count;
}

/* Nearest stdu r1 at or before insn_addr, at most DS45_SCAN_PROLOGUE_RANGE
 * bytes back and never below the window. 0 if there is none. */
static uint64_t find_prologue(uint64_t insn_addr)
{
	static const ds45_insn_match_t stdu = DS45_INSN_STDU_R1;
	uint64_t lo, a, q = 0, q_addr = 0;

	lo = (insn_addr - DS45_LV2_BASE >= DS45_SCAN_PROLOGUE_RANGE)
	     ? insn_addr - DS45_SCAN_PROLOGUE_RANGE : DS45_LV2_BASE;
	for (a = insn_addr; ; a -= 4) {
		uint32_t w;

		if (q_addr != (a & ~7ULL)) {       /* q_addr 0 = nothing read yet */
			q_addr = a & ~7ULL;
			if (ds45_lv2_read(q_addr, &q) != 0)
				return 0;
		}
		w = (a & 4) ? (uint32_t)q : (uint32_t)(q >> 32);
		if (insn_is(&stdu, w))
			return a;
		if (a <= lo)
			return 0;
	}
}

/* One walk over the window: every hit with a start but no TOC gets the second
 * qword of the first aligned pair { start, toc } with toc inside the window. */
static int resolve_tocs(scan_hit_t *hits, int n)
{
	uint64_t a, q, prev = 0;
	int i, pending = 0;

	for (i = 0; i < n; i++)
		if (hits[i].start)
			pending++;
	for (a = DS45_LV2_BASE; a < DS45_LV2_BOUND && pending; a += 8) {
		if (ds45_lv2_read(a, &q) != 0)
			return -1;
		/* start 0 (no prologue) never pairs, so neither a zero qword nor
		 * the initial prev can match */
		if (q >= DS45_LV2_BASE && q < DS45_LV2_BOUND)
			for (i = 0; i < n; i++)
				if (hits[i].start && hits[i].start == prev && !hits[i].toc) {
					hits[i].toc = q;
					pending--;
				}
		prev = q;
	}
	return 0;
}

int kernel_scan(const ds45_scan_pattern_t *pat, ds45_kernel_t *out)
{
	scan_hit_t hits[DS45_SCAN_MAX_HITS];
	uint32_t ring[DS45_SCAN_MAX_INSNS];
	uint32_t nwords = 0;
	uint64_t a, q;
	uint64_t r_fp = 0, r_hash = 0, r_slot = 0;
	uint32_t r_off = 0;
	int nhits = 0, found = 0, i, half;

	if (!pattern_valid(pat) || !out)
		return -1;

	/* Pass 1: one walk, every qword read once. The last count words are kept
	 * in a ring, so a pattern straddling qwords is matched without
	 * re-reading. */
	for (a = DS45_LV2_BASE; a < DS45_LV2_BOUND; a += 8) {
		if (ds45_lv2_read(a, &q) != 0)
			return -1;
		for (half = 0; half < 2; half++) {
			uint32_t first;
			int32_t off;

			ring[nwords & RING_MASK] = half ? (uint32_t)q : (uint32_t)(q >> 32);
			nwords++;
			if (nwords < (uint32_t)pat->count)
				continue;
			first = nwords - (uint32_t)pat->count;
			for (i = 0; i < pat->count; i++)
				if (!insn_is(&pat->insns[i], ring[(first + (uint32_t)i) & RING_MASK]))
					break;
			if (i < pat->count)
				continue;

			off = simm16(ring[(first + (uint32_t)pat->report_off_index) & RING_MASK]);
			if (off < 0)
				continue;                  /* dropped, not counted */
			if (nhits == DS45_SCAN_MAX_HITS)
				return -1;                 /* too many: ambiguous */
			hits[nhits].insns_addr = a + 4 * (uint64_t)half -
			                         4 * (uint64_t)(pat->count - 1);
			hits[nhits].start = 0;
			hits[nhits].toc = 0;
			hits[nhits].disp =
			    simm16(ring[(first + (uint32_t)pat->toc_ld_index) & RING_MASK] & 0xFFFC);
			hits[nhits].report_off = (uint32_t)off;
			nhits++;
		}
	}
	if (nhits == 0)
		return -1;

	/* Post-hit lookups may re-read: the prologue search per hit, then a
	 * second window walk for the descriptors. */
	for (i = 0; i < nhits; i++)
		hits[i].start = find_prologue(hits[i].insns_addr);
	if (resolve_tocs(hits, nhits) != 0)
		return -1;

	for (i = 0; i < nhits; i++) {
		uint64_t slot, fp;

		if (!hits[i].toc)
			continue;
		slot = hits[i].toc + (uint64_t)(int64_t)hits[i].disp;
		if ((slot & 7) || slot < DS45_LV2_BASE || slot > DS45_LV2_BOUND - 8)
			continue;
		fp = (hits[i].insns_addr + 4 * (uint64_t)pat->toc_ld_index) & ~7ULL;
		if (fp > DS45_LV2_BOUND - DS45_FP_LEN)
			continue;   /* fp >= BASE already: insns_addr is in the window */

		if (found) {
			if (slot != r_slot || hits[i].report_off != r_off)
				return -1;                 /* two distinct results */
			continue;
		}
		{
			uint8_t buf[DS45_FP_LEN];

			if (ds45_lv2_read_bytes(fp, buf, DS45_FP_LEN) != 0)
				continue;
			r_hash = ds45_fnv1a64(buf, DS45_FP_LEN);
		}
		r_fp = fp;
		r_slot = slot;
		r_off = hits[i].report_off;
		found = 1;
	}
	if (!found)
		return -1;

	out->name       = "scan";
	out->fp_addr    = r_fp;
	out->fp_hash    = r_hash;
	out->slot_addr  = r_slot;
	out->report_off = r_off;
	return 0;
}
