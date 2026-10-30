/* SOURCES: lv2.c kernel.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kernel.h"

/* Synthetic image only: hand-encoded PPC64 words and made-up fingerprint
 * bytes, no kernel bytes. */

#define B         DS45_LV2_BASE
#define LEG_SLOT  0x800000000034C530ULL      /* the built-in legacy entry */
#define FP_TAB    (B + 0x123400)
#define FP_CACHE  (B + 0x124400)
#define TAB_SLOT  (B + 0x345670)
#define CACHE_SLOT (B + 0x3A2B10)

/* scan image: pattern in a function at FUNC, its OPD at OPD */
#define FUNC      (B + 0x200000)
#define OPD       (B + 0x300000)
#define SCAN_TOC  0x8000000000360000ULL
#define SCAN_SLOT (SCAN_TOC - 0x3680)        /* 0x800000000035C980 */

#define STDU      0xF821FF81u   /* stdu r1,-0x80(r1) */
#define LD_TOC    0xE922C980u   /* ld r9,-0x3680(r2) */
#define LD_R9     0xE9290000u   /* ld r9,0(r9) */
#define ADDI_E8   0x386900E8u   /* addi r3,r9,0xE8 */
#define ADDI_E4   0x386900E4u   /* addi r3,r9,0xE4: report offset not 8-aligned */

static const ds45_insn_match_t pat_insns[] = {
	DS45_INSN_LD_FROM_R2,
	{ 0xFFFFFFFFu, LD_R9 },
	{ 0xFFFF0000u, 0x38690000u },
};
static const ds45_scan_pattern_t pat = { pat_insns, 3, 0, 2 };
static const ds45_scan_pattern_t no_pat = { NULL, 0, 0, 0 };

#define EXPECT(e) do { int l_ = (e); if (l_) return l_; } while (0)

static void put32(uint64_t a, uint32_t w)
{
	uint8_t b[4];
	b[0] = (uint8_t)(w >> 24); b[1] = (uint8_t)(w >> 16);
	b[2] = (uint8_t)(w >> 8);  b[3] = (uint8_t)w;
	fake_lv2_set(a, b, 4);
}

static void put64(uint64_t a, uint64_t v)
{
	put32(a, (uint32_t)(v >> 32));
	put32(a + 4, (uint32_t)v);
}

static void put_scan_image(uint32_t addi)
{
	put32(FUNC, STDU);
	put32(FUNC + 0x40, LD_TOC);
	put32(FUNC + 0x44, LD_R9);
	put32(FUNC + 0x48, addi);
	put64(OPD, FUNC);
	put64(OPD + 8, SCAN_TOC);
}

static uint64_t plant_fp(uint64_t addr, uint8_t seed)
{
	uint8_t fp[DS45_FP_LEN];
	int i;
	for (i = 0; i < DS45_FP_LEN; i++)
		fp[i] = (uint8_t)(seed + i * 13);
	fake_lv2_set(addr, fp, DS45_FP_LEN);
	return ds45_fnv1a64(fp, DS45_FP_LEN);
}

/* a cache file whose fingerprint matches the image */
static void store_cache(uint64_t slot, uint32_t off)
{
	ds45_kernel_t k;
	memset(&k, 0, sizeof(k));
	k.name = "x";
	k.fp_addr = FP_CACHE;
	k.fp_hash = plant_fp(FP_CACHE, 0x5A);
	k.slot_addr = slot;
	k.report_off = off;
	kernel_cache_store(&k);
}

static ds45_kernel_t g_tab[2];

/* g_tab[0] = legacy, g_tab[1] = fingerprinted entry matching the image */
static void make_table(int with_hit)
{
	memset(g_tab, 0, sizeof(g_tab));
	g_tab[0].name = "legacy";
	g_tab[0].slot_addr = LEG_SLOT;
	g_tab[0].report_off = 0xE8;
	g_tab[1].name = "tab";
	g_tab[1].fp_addr = FP_TAB;
	g_tab[1].fp_hash = plant_fp(FP_TAB, 0x11) ^ (with_hit ? 0 : 1);
	g_tab[1].slot_addr = TAB_SLOT;
	g_tab[1].report_off = 0x100;
}

static int expect_active(ds45_kmatch_t src, uint64_t slot, uint32_t off)
{
	const ds45_kernel_t *a = kernel_active();
	CHECK(kernel_active_source() == src);
	CHECK(a != NULL);
	CHECK(a->slot_addr == slot);
	CHECK(a->report_off == off);
	return 0;
}

static int test_order_table_first(void)
{
	ds45_kernel_t out;
	const char *before;

	fake_reset_all();
	make_table(1);
	store_cache(CACHE_SLOT, 0xF0);
	put_scan_image(ADDI_E8);
	before = fake_file_get(DS45_KERNEL_CACHE_FILE);
	CHECK(before != NULL);

	memset(&out, 0, sizeof(out));
	CHECK(kernel_resolve_from(g_tab, 2, &pat, &out) == DS45_KMATCH_TABLE);
	CHECK(out.slot_addr == TAB_SLOT && out.report_off == 0x100);
	EXPECT(expect_active(DS45_KMATCH_TABLE, TAB_SLOT, 0x100));
	/* a table hit neither rewrites nor deletes the cache */
	CHECK(fake_file_get(DS45_KERNEL_CACHE_FILE) == before);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_order_cache_before_scan(void)
{
	ds45_kernel_t out;

	fake_reset_all();
	make_table(0);                       /* fingerprinted entry misses */
	store_cache(CACHE_SLOT, 0xF0);
	put_scan_image(ADDI_E8);

	memset(&out, 0, sizeof(out));
	CHECK(kernel_resolve_from(g_tab, 2, &pat, &out) == DS45_KMATCH_CACHE);
	CHECK(out.slot_addr == CACHE_SLOT && out.report_off == 0xF0);
	CHECK(strcmp(out.name, "cached") == 0);
	EXPECT(expect_active(DS45_KMATCH_CACHE, CACHE_SLOT, 0xF0));
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_scan_writes_cache(void)
{
	ds45_kernel_t out, c;

	fake_reset_all();
	make_table(0);
	put_scan_image(ADDI_E8);
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);

	memset(&out, 0, sizeof(out));
	CHECK(kernel_resolve_from(g_tab, 2, &pat, &out) == DS45_KMATCH_SCAN);
	CHECK(out.slot_addr == SCAN_SLOT && out.report_off == 0xE8);
	EXPECT(expect_active(DS45_KMATCH_SCAN, SCAN_SLOT, 0xE8));

	/* the cache now holds the scan result, and the next boot takes it */
	memset(&c, 0, sizeof(c));
	CHECK(kernel_cache_load(&c) == 0);
	CHECK(c.slot_addr == SCAN_SLOT && c.report_off == 0xE8);
	CHECK(c.fp_addr == out.fp_addr && c.fp_hash == out.fp_hash);
	CHECK(kernel_resolve_from(g_tab, 2, &pat, &out) == DS45_KMATCH_CACHE);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_legacy_last(void)
{
	ds45_kernel_t out;

	fake_reset_all();
	make_table(0);
	memset(&out, 0, sizeof(out));
	CHECK(kernel_resolve_from(g_tab, 2, &pat, &out) == DS45_KMATCH_LEGACY);
	CHECK(out.slot_addr == LEG_SLOT && out.report_off == 0xE8);
	EXPECT(expect_active(DS45_KMATCH_LEGACY, LEG_SLOT, 0xE8));
	/* a failed scan is never cached */
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);

	/* an invalid scan pattern falls through to legacy without any read */
	fake_reset_all();
	make_table(0);
	CHECK(kernel_resolve_from(g_tab, 1, &no_pat, NULL) == DS45_KMATCH_LEGACY);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_none_when_all_fail(void)
{
	ds45_kernel_t out;
	unsigned char *p = (unsigned char *)&out;
	size_t i;

	/* something active first, so NONE has to clear it */
	fake_reset_all();
	make_table(1);
	CHECK(kernel_resolve_from(g_tab, 2, &no_pat, NULL) == DS45_KMATCH_TABLE);
	CHECK(kernel_active() != NULL);

	fake_reset_all();
	make_table(0);
	memset(&out, 0xAA, sizeof(out));
	/* only the fingerprinted (missing) entry: no legacy fallback */
	CHECK(kernel_resolve_from(&g_tab[1], 1, &pat, &out) == DS45_KMATCH_NONE);
	CHECK(kernel_active() == NULL);
	CHECK(kernel_active_source() == DS45_KMATCH_NONE);
	for (i = 0; i < sizeof(out); i++)
		CHECK(p[i] == 0xAA);
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);

	/* empty table, NULL table */
	CHECK(kernel_resolve_from(g_tab, 0, &no_pat, NULL) == DS45_KMATCH_NONE);
	CHECK(kernel_resolve_from(NULL, 3, NULL, NULL) == DS45_KMATCH_NONE);
	CHECK(kernel_active() == NULL);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_reject_deletes_cache(void)
{
	fake_reset_all();
	make_table(0);
	store_cache(CACHE_SLOT, 0xF0);
	CHECK(kernel_resolve_from(g_tab, 2, &no_pat, NULL) == DS45_KMATCH_CACHE);
	CHECK(kernel_active() != NULL);

	kernel_reject_active();
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);
	CHECK(kernel_active() == NULL);
	CHECK(kernel_active_source() == DS45_KMATCH_NONE);

	/* a rejected legacy record also clears the active record */
	fake_reset_all();
	make_table(0);
	CHECK(kernel_resolve_from(g_tab, 1, &no_pat, NULL) == DS45_KMATCH_LEGACY);
	kernel_reject_active();
	CHECK(kernel_active() == NULL);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	return 0;
}

/* A cached record whose fingerprint still matches but whose slot or report
 * offset is unusable is a miss, and is deleted. */
static int cache_bad_case(uint64_t slot, uint32_t off)
{
	fake_reset_all();
	make_table(0);
	store_cache(slot, off);
	CHECK(kernel_resolve_from(g_tab, 2, &no_pat, NULL) == DS45_KMATCH_LEGACY);
	EXPECT(expect_active(DS45_KMATCH_LEGACY, LEG_SLOT, 0xE8));
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_cache_bad_record_is_miss(void)
{
	EXPECT(cache_bad_case(CACHE_SLOT + 4, 0xF0));            /* unaligned slot */
	EXPECT(cache_bad_case(DS45_LV2_BOUND, 0xF0));            /* at the bound */
	EXPECT(cache_bad_case(DS45_LV2_BOUND - 4, 0xF0));
	EXPECT(cache_bad_case(DS45_LV2_BASE - 8, 0xF0));         /* below the window */
	EXPECT(cache_bad_case(0, 0xF0));
	EXPECT(cache_bad_case(CACHE_SLOT, 0xF4));                /* unaligned offset */
	EXPECT(cache_bad_case(CACHE_SLOT, DS45_REPORT_OFF_MAX + 8));
	EXPECT(cache_bad_case(CACHE_SLOT, 0xFFFFFFF8u));

	/* the edges themselves are valid */
	fake_reset_all();
	make_table(0);
	store_cache(DS45_LV2_BOUND - 8, DS45_REPORT_OFF_MAX);
	CHECK(kernel_resolve_from(g_tab, 2, &no_pat, NULL) == DS45_KMATCH_CACHE);
	EXPECT(expect_active(DS45_KMATCH_CACHE, DS45_LV2_BOUND - 8, DS45_REPORT_OFF_MAX));
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 0);

	fake_reset_all();
	make_table(0);
	store_cache(DS45_LV2_BASE, 0);
	CHECK(kernel_resolve_from(g_tab, 2, &no_pat, NULL) == DS45_KMATCH_CACHE);
	return 0;
}

static int test_scan_bad_record_not_cached(void)
{
	fake_reset_all();
	make_table(0);
	put_scan_image(ADDI_E4);             /* scan finds it, but 0xE4 is unusable */
	CHECK(kernel_resolve_from(g_tab, 2, &pat, NULL) == DS45_KMATCH_LEGACY);
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_builtin_resolve(void)
{
	ds45_kernel_t out;

	fake_reset_all();
	memset(&out, 0, sizeof(out));
	CHECK(kernel_resolve(&out) == DS45_KMATCH_LEGACY);
	CHECK(out.slot_addr == LEG_SLOT && out.report_off == 0xE8);
	EXPECT(expect_active(DS45_KMATCH_LEGACY, LEG_SLOT, 0xE8));
	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static uint64_t sc8_ok(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
	(void)a4;
	if (a1 != 0x7777)
		return (uint64_t)-1;
	if (a2 == 0x0013) {
		memcpy((void *)(uintptr_t)a3, "DEX", 4);
		return 0;
	}
	if (a2 == 0x0014)
		return 0x4920;
	return (uint64_t)-1;
}

static int test_log_fw(void)
{
	int i, saw13 = 0, saw14 = 0;

	fake_reset_all();
	fake_syscall8 = sc8_ok;
	kernel_log_fw();
	CHECK(fake_sc8_calls == 2);
	for (i = 0; i < 2; i++) {
		CHECK(fake_sc8_a1[i] == 0x7777);
		saw13 |= fake_sc8_a2[i] == 0x0013;
		saw14 |= fake_sc8_a2[i] == 0x0014;
	}
	CHECK(saw13 && saw14);

	/* no PS3MAPI (syscall 8 errors): logging only, resolution unaffected.
	 * The only reads are the fingerprint checks (32 bytes = 4 peeks each)
	 * of the real table, none of which matches the empty fake image. */
	fake_reset_all();
	kernel_log_fw();
	CHECK(fake_sc8_calls == 2);
	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	{
		int fps = 0;

		for (i = 0; i < ds45_kernel_table_count; i++)
			if (ds45_kernel_table[i].fp_addr != 0)
				fps++;
		CHECK(fake_lv2_peek_count == fps * (DS45_FP_LEN / 8));
	}
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

int run_tests(void)
{
	EXPECT(test_order_table_first());
	EXPECT(test_order_cache_before_scan());
	EXPECT(test_scan_writes_cache());
	EXPECT(test_legacy_last());
	EXPECT(test_none_when_all_fail());
	EXPECT(test_reject_deletes_cache());
	EXPECT(test_cache_bad_record_is_miss());
	EXPECT(test_scan_bad_record_not_cached());
	EXPECT(test_builtin_resolve());
	EXPECT(test_log_fw());
	return 0;
}
