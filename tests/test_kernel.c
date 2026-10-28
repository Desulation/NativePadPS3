/* SOURCES: lv2.c kernel.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kernel.h"

#define FP_ADDR 0x8000000000123400ULL
#define GOOD_TAIL " 0x80000000003a2b10 0xe8"

static uint8_t g_fp[DS45_FP_LEN];

static uint64_t plant_fp(uint64_t addr, uint8_t seed)
{
	int i;
	for (i = 0; i < DS45_FP_LEN; i++)
		g_fp[i] = (uint8_t)(seed + i * 7);
	fake_lv2_set(addr, g_fp, DS45_FP_LEN);
	return ds45_fnv1a64(g_fp, DS45_FP_LEN);
}

/* "0x" + 16 lowercase hex digits, independent of the code under test */
static void hex16(char *dst, uint64_t v)
{
	static const char hex[] = "0123456789abcdef";
	int s, n = 0;
	dst[n++] = '0'; dst[n++] = 'x';
	for (s = 60; s >= 0; s -= 4)
		dst[n++] = hex[(v >> s) & 15];
	dst[n] = 0;
}

static int load_text(const char *text)
{
	ds45_kernel_t out;
	fake_file_set(DS45_KERNEL_CACHE_FILE, text);
	return kernel_cache_load(&out);
}

static int test_table_hit(void)
{
	ds45_kernel_t tab[3];
	uint64_t h;

	fake_reset_all();
	h = plant_fp(FP_ADDR, 0x11);
	memset(tab, 0, sizeof(tab));
	/* legacy entry (no fingerprint) must be skipped, even if first */
	tab[0].name = "legacy"; tab[0].slot_addr = 0x8000000000001000ULL;
	tab[1].name = "other";  tab[1].fp_addr = FP_ADDR; tab[1].fp_hash = h ^ 1;
	tab[2].name = "hit";    tab[2].fp_addr = FP_ADDR; tab[2].fp_hash = h;
	tab[2].slot_addr = 0x8000000000345678ULL; tab[2].report_off = 0xE8;
	CHECK(kernel_match_table(tab, 3) == &tab[2]);
	CHECK(kernel_fingerprint_matches(FP_ADDR, h) == 1);
	CHECK(kernel_match_table(tab, 0) == NULL);
	CHECK(kernel_match_table(NULL, 0) == NULL);
	return 0;
}

static int test_table_miss_wrong_hash(void)
{
	ds45_kernel_t tab[1];
	uint64_t h;

	fake_reset_all();
	h = plant_fp(FP_ADDR, 0x22);
	memset(tab, 0, sizeof(tab));
	tab[0].name = "x"; tab[0].fp_addr = FP_ADDR; tab[0].fp_hash = h + 1;
	CHECK(kernel_match_table(tab, 1) == NULL);
	CHECK(kernel_fingerprint_matches(FP_ADDR, h + 1) == 0);
	/* fp_addr == 0 never matches, whatever the hash */
	CHECK(kernel_fingerprint_matches(0, 0) == 0);
	return 0;
}

static int test_table_fp_out_of_window(void)
{
	ds45_kernel_t tab[4];

	fake_reset_all();
	memset(tab, 0, sizeof(tab));
	tab[0].name = "bound";     tab[0].fp_addr = DS45_LV2_BOUND;
	tab[1].name = "straddle";  tab[1].fp_addr = DS45_LV2_BOUND - 8; /* 32 bytes cross the end */
	tab[2].name = "unaligned"; tab[2].fp_addr = FP_ADDR + 4;
	tab[3].name = "below";     tab[3].fp_addr = DS45_LV2_BASE - 8;
	CHECK(kernel_match_table(tab, 4) == NULL);
	CHECK(kernel_fingerprint_matches(DS45_LV2_BOUND, 0) == 0);
	CHECK(kernel_fingerprint_matches(0xFFFFFFFFFFFFFFF8ULL, 0) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_table_contents(void)
{
	const ds45_kernel_t *leg = NULL;
	int i;

	CHECK(ds45_kernel_table_count >= 1);
	for (i = 0; i < ds45_kernel_table_count; i++)
		if (ds45_kernel_table[i].fp_addr == 0)
			leg = &ds45_kernel_table[i];
	CHECK(leg != NULL);
	CHECK(leg->fp_hash == 0);
	CHECK(leg->slot_addr == 0x800000000034C530ULL);
	CHECK(leg->report_off == 0xE8);
	return 0;
}

static int test_cache_roundtrip(void)
{
	ds45_kernel_t k, out;
	uint64_t h;

	fake_reset_all();
	h = plant_fp(FP_ADDR, 0x33);
	memset(&k, 0, sizeof(k));
	k.name = "scan"; k.fp_addr = FP_ADDR; k.fp_hash = h;
	k.slot_addr = 0x80000000003A2B10ULL; k.report_off = 0x1234;
	CHECK(kernel_cache_store(&k) == 0);

	memset(&out, 0xAA, sizeof(out));
	CHECK(kernel_cache_load(&out) == 0);
	CHECK(out.fp_addr == FP_ADDR);
	CHECK(out.fp_hash == h);
	CHECK(out.slot_addr == 0x80000000003A2B10ULL);
	CHECK(out.report_off == 0x1234);
	CHECK(out.name != NULL && strcmp(out.name, "cached") == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_cache_exact_format(void)
{
	ds45_kernel_t k;

	fake_reset_all();
	memset(&k, 0, sizeof(k));
	k.fp_addr = 0x8000000000123400ULL; k.fp_hash = 0xDEADBEEF0123ULL;
	k.slot_addr = 0x80000000003A2B10ULL; k.report_off = 0xE8;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(strcmp(fake_file_get(DS45_KERNEL_CACHE_FILE),
	  "DS45K1 0x8000000000123400 0xdeadbeef0123 0x80000000003a2b10 0xe8\n") == 0);

	/* zero values print as 0x0; a rewrite replaces the old content */
	k.fp_hash = 0; k.report_off = 0;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(strcmp(fake_file_get(DS45_KERNEL_CACHE_FILE),
	  "DS45K1 0x8000000000123400 0x0 0x80000000003a2b10 0x0\n") == 0);
	return 0;
}

static int test_cache_store_rejects_no_fingerprint(void)
{
	ds45_kernel_t k;

	fake_reset_all();
	memset(&k, 0, sizeof(k));
	k.slot_addr = 0x800000000034C530ULL; k.report_off = 0xE8;
	CHECK(kernel_cache_store(&k) == -1);
	CHECK(kernel_cache_store(NULL) == -1);
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);
	return 0;
}

static int test_cache_stale_fingerprint(void)
{
	ds45_kernel_t k, out;
	uint64_t h;
	uint8_t other[DS45_FP_LEN];

	fake_reset_all();
	h = plant_fp(FP_ADDR, 0x44);
	memset(&k, 0, sizeof(k));
	k.fp_addr = FP_ADDR; k.fp_hash = h;
	k.slot_addr = 0x80000000003A2B10ULL; k.report_off = 0xE8;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(kernel_cache_load(&out) == 0);

	/* kernel changed: bytes at the fingerprint address differ now */
	memset(other, 0x5A, sizeof(other));
	fake_lv2_set(FP_ADDR, other, sizeof(other));
	CHECK(kernel_cache_load(&out) == -1);

	/* stored hash simply wrong */
	fake_lv2_set(FP_ADDR, g_fp, DS45_FP_LEN);
	k.fp_hash = h ^ 0x10;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(kernel_cache_load(&out) == -1);

	/* fingerprint address outside the window: no peek, no match */
	k.fp_addr = DS45_LV2_BOUND; k.fp_hash = h;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(kernel_cache_load(&out) == -1);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_cache_malformed_lines(void)
{
	ds45_kernel_t out;
	char good[160], buf[256], hh[24];
	uint64_t h;

	fake_reset_all();
	h = plant_fp(FP_ADDR, 0x55);

	/* sanity: a hand-built good line for the planted fingerprint loads, so
	 * every rejection below is down to the text and not the fixture. */
	hex16(hh, h);
	strcpy(good, "DS45K1 0x8000000000123400 ");
	strcat(good, hh);
	strcat(good, GOOD_TAIL);               /* no newline yet */
	strcpy(buf, good); strcat(buf, "\n");
	CHECK(load_text(buf) == 0);

	/* policy: a line without its trailing newline is REJECTED, so a line
	 * truncated inside its last field cannot parse as a shorter value */
	CHECK(load_text(good) == -1);
	/* trailing junk after the newline, CRLF, blank lines, a second line */
	strcpy(buf, good); strcat(buf, "\nx");
	CHECK(load_text(buf) == -1);
	strcpy(buf, good); strcat(buf, "\r\n");
	CHECK(load_text(buf) == -1);
	strcpy(buf, good); strcat(buf, "\n\n");
	CHECK(load_text(buf) == -1);
	strcpy(buf, good); strcat(buf, "\n"); strcat(buf, good); strcat(buf, "\n");
	CHECK(load_text(buf) == -1);
	strcpy(buf, good); strcat(buf, " \n");
	CHECK(load_text(buf) == -1);                               /* trailing space */
	strcpy(buf, good); strcat(buf, " 0x1\n");
	CHECK(load_text(buf) == -1);                               /* extra field */

	CHECK(load_text("") == -1);
	CHECK(load_text("\n") == -1);
	CHECK(load_text("DS45K0 0x8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);   /* wrong magic */
	CHECK(load_text("ds45k1 0x8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);
	CHECK(load_text("0x8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);          /* no magic */
	CHECK(load_text("DS45K1\n") == -1);
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003a2b10\n") == -1);        /* missing field */
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003a2b10 0x\n") == -1);     /* cut mid-field */
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x8000000000") == -1);                /* cut, no newline */
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003a2b10 0xe8 0x1\n") == -1);
	CHECK(load_text("DS45K1  0x8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);  /* double space */
	CHECK(load_text("DS45K1 8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);     /* no 0x */
	CHECK(load_text("DS45K1 0x8000000000123400 0xZZ 0x80000000003a2b10 0xe8\n") == -1);  /* non-hex */
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003A2B10 0xe8\n") == -1);   /* uppercase */
	CHECK(load_text("DS45K1 0X8000000000123400 0x1 0x80000000003a2b10 0xe8\n") == -1);   /* 0X */
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003a2b10 -1\n") == -1);
	CHECK(load_text("DS45K1 0x8000000000123400 0x1 0x80000000003a2b10 0x1ffffffff\n") == -1); /* off > 32 bit */
	CHECK(load_text("DS45K1 0x80000000001234001 0x1 0x80000000003a2b10 0xe8\n") == -1);  /* 17 digits */
	CHECK(load_text("DS45K1 0x0 0x0 0x80000000003a2b10 0xe8\n") == -1);                  /* fp_addr == 0 */

	/* no file at all, NULL out */
	fake_reset_all();
	plant_fp(FP_ADDR, 0x55);
	CHECK(kernel_cache_load(&out) == -1);
	CHECK(kernel_cache_load(NULL) == -1);

	/* oversized garbage file (longer than any read buffer) */
	{
		static char big[1024];
		memset(big, 'a', sizeof(big) - 1);
		CHECK(load_text(big) == -1);
	}
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_cache_load_leaves_out_untouched_on_failure(void)
{
	ds45_kernel_t out;
	unsigned char *p = (unsigned char *)&out;
	size_t i;

	fake_reset_all();
	memset(&out, 0xAA, sizeof(out));
	fake_file_set(DS45_KERNEL_CACHE_FILE, "garbage\n");
	CHECK(kernel_cache_load(&out) == -1);
	for (i = 0; i < sizeof(out); i++)
		CHECK(p[i] == 0xAA);
	return 0;
}

static int test_cache_delete(void)
{
	ds45_kernel_t k;

	fake_reset_all();
	memset(&k, 0, sizeof(k));
	k.fp_addr = FP_ADDR; k.fp_hash = 1; k.slot_addr = 0x80000000003A2B10ULL;
	CHECK(kernel_cache_store(&k) == 0);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 0);
	kernel_cache_delete();
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	return 0;
}

int run_tests(void)
{
	int r;
#define RUN(t) do { r = t(); if (r) return r; } while (0)
	RUN(test_table_hit);
	RUN(test_table_miss_wrong_hash);
	RUN(test_table_fp_out_of_window);
	RUN(test_table_contents);
	RUN(test_cache_roundtrip);
	RUN(test_cache_exact_format);
	RUN(test_cache_store_rejects_no_fingerprint);
	RUN(test_cache_stale_fingerprint);
	RUN(test_cache_malformed_lines);
	RUN(test_cache_load_leaves_out_untouched_on_failure);
	RUN(test_cache_delete);
	return 0;
}
