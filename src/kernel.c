/*
 * NPP - kernel table matching and the validated cache. See kernel.h.
 */

#include <stddef.h>
#ifdef NPP_DEBUG
#include <sys/process.h>   /* system_call_N: PS3MAPI firmware query */
#include <sys/sys_time.h>  /* scan timing */
#endif

#include "lv2.h"
#include "kernel.h"

/* Fingerprinted entries are added here (and wrapped in NPP_NO_TABLE, a
 * test-only define). The legacy entry has no fingerprint and is always
 * present: 0x34FBB0 - 0x3680. */
const npp_kernel_t npp_kernel_table[] = {
#ifndef NPP_NO_TABLE
	/* DEX 4.92 (Evilnat): TOC 0x3759C0 - 0x35E0 holds the Bluetooth HID
	 * object (0x4325E0); its live report is at +0xE8. Found by diffing two
	 * dumps of the window taken with a DualSense connected, untouched and
	 * with Cross held. Fingerprint: the first `ld r3,-0x35E0(r2)`. */
	{ "DEX 4.92", 0x80000000000F3380ULL, 0xDF192C9D78181BF7ULL, 0x80000000003723E0ULL, 0xE8 },
#endif
	{ "CEX 4.93 (legacy)", 0, 0, 0x800000000034C530ULL, 0xE8 },
};
const int npp_kernel_table_count =
	(int)(sizeof(npp_kernel_table) / sizeof(npp_kernel_table[0]));

int kernel_fingerprint_matches(uint64_t fp_addr, uint64_t fp_hash)
{
	uint8_t buf[NPP_FP_LEN];

	if (fp_addr == 0)
		return 0;
	if (npp_lv2_read_bytes(fp_addr, buf, NPP_FP_LEN) != 0)
		return 0;
	return npp_fnv1a64(buf, NPP_FP_LEN) == fp_hash;
}

const npp_kernel_t *kernel_match_table(const npp_kernel_t *tab, int n)
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

int kernel_cache_load(npp_kernel_t *out)
{
	char buf[160];
	const char *p, *end;
	uint64_t fp_addr, fp_hash, slot, off;
	int n, i;

	if (!out)
		return -1;
	/* The real npp_read_file does not NUL-terminate. */
	n = npp_read_file(npp_path(NPP_PATH_KERNEL_CACHE), buf, (int)sizeof(buf) - 1);
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

int kernel_cache_store(const npp_kernel_t *k)
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
	return npp_write_file(npp_path(NPP_PATH_KERNEL_CACHE), buf, (int)(p - buf));
}

void kernel_cache_delete(void)
{
	npp_delete_file(npp_path(NPP_PATH_KERNEL_CACHE));
}

/* ------------------------------------------------------------------------ */
/* signature scan                                                            */
/* ------------------------------------------------------------------------ */

/* The real pattern comes from a later task; until then the scan never runs. */
const npp_scan_pattern_t npp_scan_pattern = { NULL, 0, 0, 0 };

#define RING_MASK (NPP_SCAN_MAX_INSNS - 1)
typedef char npp_scan_ring_is_pow2[(NPP_SCAN_MAX_INSNS & RING_MASK) == 0 ? 1 : -1];

typedef struct {
	uint64_t insns_addr;   /* address of insns[0] */
	uint64_t start;        /* function start (its stdu); 0 = none */
	uint64_t toc;          /* from the descriptor; 0 = none */
	int32_t  disp;
	uint32_t report_off;
} scan_hit_t;

static int insn_is(const npp_insn_match_t *m, uint32_t w)
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

static int pattern_valid(const npp_scan_pattern_t *pat)
{
	return pat && pat->insns && pat->count > 0 &&
	       pat->count <= NPP_SCAN_MAX_INSNS &&
	       pat->toc_ld_index >= 0 && pat->toc_ld_index < pat->count &&
	       pat->report_off_index >= 0 && pat->report_off_index < pat->count;
}

/* Nearest stdu r1 at or before insn_addr, at most NPP_SCAN_PROLOGUE_RANGE
 * bytes back and never below the window. 0 if there is none. */
static uint64_t find_prologue(uint64_t insn_addr)
{
	static const npp_insn_match_t stdu = NPP_INSN_STDU_R1;
	uint64_t lo, a, q = 0, q_addr = 0;

	lo = (insn_addr - NPP_LV2_BASE >= NPP_SCAN_PROLOGUE_RANGE)
	     ? insn_addr - NPP_SCAN_PROLOGUE_RANGE : NPP_LV2_BASE;
	for (a = insn_addr; ; a -= 4) {
		uint32_t w;

		if (q_addr != (a & ~7ULL)) {       /* q_addr 0 = nothing read yet */
			q_addr = a & ~7ULL;
			if (npp_lv2_read(q_addr, &q) != 0)
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
	for (a = NPP_LV2_BASE; a < NPP_LV2_BOUND && pending; a += 8) {
		if (npp_lv2_read(a, &q) != 0)
			return -1;
		/* start 0 (no prologue) never pairs, so neither a zero qword nor
		 * the initial prev can match */
		if (q >= NPP_LV2_BASE && q < NPP_LV2_BOUND)
			for (i = 0; i < n; i++)
				if (hits[i].start && hits[i].start == prev && !hits[i].toc) {
					hits[i].toc = q;
					pending--;
				}
		prev = q;
	}
	return 0;
}

int kernel_scan(const npp_scan_pattern_t *pat, npp_kernel_t *out)
{
	scan_hit_t hits[NPP_SCAN_MAX_HITS];
	uint32_t ring[NPP_SCAN_MAX_INSNS];
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
	for (a = NPP_LV2_BASE; a < NPP_LV2_BOUND; a += 8) {
		if (npp_lv2_read(a, &q) != 0)
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
			if (nhits == NPP_SCAN_MAX_HITS)
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
		if ((slot & 7) || slot < NPP_LV2_BASE || slot > NPP_LV2_BOUND - 8)
			continue;
		fp = (hits[i].insns_addr + 4 * (uint64_t)pat->toc_ld_index) & ~7ULL;
		if (fp > NPP_LV2_BOUND - NPP_FP_LEN)
			continue;   /* fp >= BASE already: insns_addr is in the window */

		if (found) {
			if (slot != r_slot || hits[i].report_off != r_off)
				return -1;                 /* two distinct results */
			continue;
		}
		{
			uint8_t buf[NPP_FP_LEN];

			if (npp_lv2_read_bytes(fp, buf, NPP_FP_LEN) != 0)
				continue;
			r_hash = npp_fnv1a64(buf, NPP_FP_LEN);
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

/* ------------------------------------------------------------------------ */
/* resolution                                                                */
/* ------------------------------------------------------------------------ */

static npp_kernel_t        g_active_rec;
static const npp_kernel_t *g_active;
static npp_kmatch_t        g_active_src = NPP_KMATCH_NONE;

int kernel_record_usable(const npp_kernel_t *k)
{
	return k && (k->slot_addr & 7) == 0 &&
	       k->slot_addr >= NPP_LV2_BASE && k->slot_addr <= NPP_LV2_BOUND - 8 &&
	       (k->report_off & 7) == 0 && k->report_off <= NPP_REPORT_OFF_MAX;
}

static npp_kmatch_t activate(const npp_kernel_t *k, npp_kmatch_t src,
                              npp_kernel_t *out)
{
	g_active_rec = *k;
	g_active     = &g_active_rec;
	g_active_src = src;
	if (out)
		*out = *k;
	NPP_TRACE_HEX("kernel_slot_lo", k->slot_addr);
	NPP_TRACE_HEX("kernel_report_off", k->report_off);
	return src;
}

#ifdef NPP_DEBUG
static uint64_t now_us(void)
{
	return (uint64_t)sys_time_get_system_time();
}
#endif

npp_kmatch_t kernel_resolve_from(const npp_kernel_t *tab, int n,
                                  const npp_scan_pattern_t *pat,
                                  npp_kernel_t *out)
{
	const npp_kernel_t *hit;
	npp_kernel_t rec;
	int i, r;
#ifdef NPP_DEBUG
	uint64_t t0;
#endif

	g_active     = NULL;
	g_active_src = NPP_KMATCH_NONE;

	/* 1. a verified fingerprint */
	hit = kernel_match_table(tab, n);
	if (hit && kernel_record_usable(hit))
		return activate(hit, NPP_KMATCH_TABLE, out);

	/* 2. the cache: its fingerprint was re-checked by the loader; a record
	 * that still cannot be read from is a miss and is not kept */
	if (kernel_cache_load(&rec) == 0) {
		if (kernel_record_usable(&rec))
			return activate(&rec, NPP_KMATCH_CACHE, out);
		NPP_TRACE("kernel_cache_unusable_deleted");
		kernel_cache_delete();
	}

	/* 3. the scan; only a usable result is cached */
#ifdef NPP_DEBUG
	t0 = now_us();
#endif
	r = kernel_scan(pat, &rec);
#ifdef NPP_DEBUG
	NPP_TRACE_HEX("kernel_scan_ms", (now_us() - t0) / 1000);
#endif
	if (r == 0 && kernel_record_usable(&rec)) {
		if (kernel_cache_store(&rec) != 0)
			NPP_TRACE("kernel_cache_store_FAILED");
		return activate(&rec, NPP_KMATCH_SCAN, out);
	}

	/* 4. the unverified legacy entry, left to the cross-check */
	if (tab)
		for (i = 0; i < n; i++)
			if (tab[i].fp_addr == 0 && kernel_record_usable(&tab[i]))
				return activate(&tab[i], NPP_KMATCH_LEGACY, out);

	return NPP_KMATCH_NONE;
}

npp_kmatch_t kernel_resolve(npp_kernel_t *out)
{
	return kernel_resolve_from(npp_kernel_table, npp_kernel_table_count,
	                           &npp_scan_pattern, out);
}

const npp_kernel_t *kernel_active(void)
{
	return g_active;
}

npp_kmatch_t kernel_active_source(void)
{
	return g_active ? g_active_src : NPP_KMATCH_NONE;
}

void kernel_reject_active(void)
{
	kernel_cache_delete();
	g_active     = NULL;
	g_active_src = NPP_KMATCH_NONE;
	NPP_TRACE("kernel_rejected");
}

/* ------------------------------------------------------------------------ */
/* firmware identification (trace only)                                      */
/* ------------------------------------------------------------------------ */

#define PS3MAPI_SYSCALL8_OPCODE     0x7777
#define PS3MAPI_OPCODE_GET_FW_TYPE  0x0013
#define PS3MAPI_OPCODE_GET_FW_VER   0x0014

#ifdef NPP_DEBUG
int kernel_fw_query(char *type, int cap, uint32_t *ver)
{
	/* Generous: Cobra copies a short string ("CEX", "DEX", ...). */
	char raw[64];
	int32_t rc;
	int i, n = 0;

	for (i = 0; i < (int)sizeof(raw); i++)
		raw[i] = 0;
	{
		system_call_3(8, PS3MAPI_SYSCALL8_OPCODE, PS3MAPI_OPCODE_GET_FW_TYPE,
		              (uint64_t)(uintptr_t)raw);
		rc = (int32_t)p1;
	}
	if (type && cap > 0) {
		/* printable characters only, and never past the end of either */
		if (rc == 0)
			for (i = 0; i < (int)sizeof(raw) && raw[i] && n < cap - 1; i++)
				if (raw[i] >= 0x20 && raw[i] < 0x7F)
					type[n++] = raw[i];
		type[n] = 0;
	}

	{
		system_call_2(8, PS3MAPI_SYSCALL8_OPCODE, PS3MAPI_OPCODE_GET_FW_VER);
		if (ver)
			*ver = (uint32_t)p1;
	}
	return rc;
}
#endif

void kernel_log_fw(void)
{
#ifdef NPP_DEBUG
	char tag[32];
	const char prefix[] = "fw_type=";
	int32_t rc;
	uint32_t ver;
	int i;

	rc = kernel_fw_query(tag + (sizeof(prefix) - 1),
	                     (int)(sizeof(tag) - (sizeof(prefix) - 1)), &ver);
	if (rc != 0) {
		NPP_TRACE_HEX("fw_type_err", rc);
	} else {
		for (i = 0; prefix[i]; i++)
			tag[i] = prefix[i];
		NPP_TRACE(tag);
	}
	NPP_TRACE_HEX("fw_version", ver);
#endif
}
