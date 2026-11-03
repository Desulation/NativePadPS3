/* SOURCES: lv2.c kernel.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kernel.h"

/* Synthetic image only: hand-encoded PPC64 instruction words, no kernel
 * bytes. Every test ends with fake_lv2_out_of_bounds == 0. */

#define B     DS45_LV2_BASE
#define END   DS45_LV2_BOUND
#define FUNC  (B + 0x200000)
#define OPD   (B + 0x300000)
#define TOC   0x800000000034FBB0ULL
#define SLOT  0x800000000034C530ULL       /* TOC - 0x3680 */

#define STDU     0xF821FF81u   /* stdu r1,-0x80(r1) */
#define MFLR     0x7C0802A6u   /* mflr r0 */
#define LD_TOC   0xE922C980u   /* ld r9,-0x3680(r2) */
#define LD_TOC2  0xE922C988u   /* ld r9,-0x3678(r2) */
#define LD_TOCU  0xE922C984u   /* ld r9,-0x367C(r2): slot not 8-aligned */
#define LD_TOCP  0xE9220010u   /* ld r9,0x10(r2) */
#define LD_R9    0xE9290000u   /* ld r9,0(r9) */
#define ADDI     0x386900E8u   /* addi r3,r9,0xE8 */
#define ADDI_NEG 0x3869FFE8u   /* addi r3,r9,-0x18 */

static const ds45_insn_match_t pat_insns[] = {
	DS45_INSN_LD_FROM_R2,
	{ 0xFFFFFFFFu, LD_R9 },
	{ 0xFFFF0000u, 0x38690000u },     /* addi r3,r9,any */
};
static const ds45_scan_pattern_t pat = { pat_insns, 3, 0, 2 };

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

/* the 3-word pattern instance: ld rT,disp(r2); ld r9,0(r9); addi r3,r9,off */
static void put_pattern(uint64_t a, uint32_t ld, uint32_t addi)
{
	put32(a, ld);
	put32(a + 4, LD_R9);
	put32(a + 8, addi);
}

/* prologue at start, pattern at pat_addr */
static void put_func(uint64_t start, uint64_t pat_addr, uint32_t ld, uint32_t addi)
{
	put32(start, STDU);
	put32(start + 4, MFLR);
	put_pattern(pat_addr, ld, addi);
}

/* LV2 OPD: { entry, toc, env } */
static void put_opd(uint64_t at, uint64_t entry, uint64_t toc)
{
	put64(at, entry);
	put64(at + 8, toc);
	put64(at + 16, 0);
}

/* Expected fingerprint, read straight from the fake (not via lv2.c). */
static uint64_t hash_at(uint64_t a)
{
	uint8_t buf[DS45_FP_LEN];
	int i, b;
	for (i = 0; i < DS45_FP_LEN; i += 8) {
		uint64_t q = fake_syscall(6, a + (uint64_t)i, 0, 0, 0);
		for (b = 0; b < 8; b++)
			buf[i + b] = (uint8_t)(q >> (56 - 8 * b));
	}
	return ds45_fnv1a64(buf, DS45_FP_LEN);
}

static int expect_result(uint64_t fp_addr, uint64_t slot, uint32_t off)
{
	ds45_kernel_t out;
	uint64_t h = hash_at(fp_addr);

	memset(&out, 0, sizeof(out));
	CHECK(kernel_scan(&pat, &out) == 0);
	CHECK(out.name != NULL && strcmp(out.name, "scan") == 0);
	CHECK(out.fp_addr == fp_addr);
	CHECK(out.fp_hash == h);
	CHECK(out.slot_addr == slot);
	CHECK(out.report_off == off);
	/* the result is self-consistent with the table/cache matcher */
	CHECK(kernel_fingerprint_matches(out.fp_addr, out.fp_hash) == 1);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int expect_fail(void)
{
	ds45_kernel_t out;
	unsigned char *p = (unsigned char *)&out;
	size_t i;

	memset(&out, 0xAA, sizeof(out));
	CHECK(kernel_scan(&pat, &out) == -1);
	for (i = 0; i < sizeof(out); i++)           /* untouched on failure */
		CHECK(p[i] == 0xAA);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

#define EXPECT(e) do { int l_ = (e); if (l_) return l_; } while (0)

static int test_scan_single_hit(void)
{
	/* pattern starts on a qword boundary */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* pattern starts mid-qword: fp_addr rounds down to the qword */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x44, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* the OPD may sit before the function in memory */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x44, LD_TOC, ADDI);
	put_opd(B + 0x1000, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));
	return 0;
}

static int test_scan_no_hit(void)
{
	fake_reset_all();
	EXPECT(expect_fail());                       /* empty image */

	/* prologue and OPD present, pattern broken in its middle word */
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put32(FUNC + 0x44, LD_R9 + 4);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());

	/* first word not an ld from r2 (ld r9,-0x3680(r3)) */
	put32(FUNC + 0x44, LD_R9);
	put32(FUNC + 0x40, 0xE923C980u);
	EXPECT(expect_fail());
	return 0;
}

static int test_scan_ambiguous(void)
{
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	put_func(FUNC + 0x10000, FUNC + 0x10040, LD_TOC2, ADDI);
	put_opd(OPD + 0x18, FUNC + 0x10000, TOC);
	EXPECT(expect_fail());

	/* same displacement, different report offset: still two results */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	put_func(FUNC + 0x10000, FUNC + 0x10040, LD_TOC, 0x386900F0u);
	put_opd(OPD + 0x18, FUNC + 0x10000, TOC);
	EXPECT(expect_fail());
	return 0;
}

static int test_scan_duplicate_hits_same_result(void)
{
	/* two functions, two OPDs, same TOC/displacement/offset: one result;
	 * the fingerprint is that of the first hit in address order */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	put_func(FUNC + 0x10000, FUNC + 0x10044, LD_TOC, ADDI);
	put_opd(OPD + 0x18, FUNC + 0x10000, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* two hits inside one function */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_pattern(FUNC + 0x80, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));
	return 0;
}

static int test_scan_no_prologue(void)
{
	/* no stdu at all */
	fake_reset_all();
	put_pattern(FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());

	/* stdu 0x804 bytes before the hit: out of range */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x804, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());

	/* stdu exactly 0x800 bytes before the hit: in range */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x800, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x800, SLOT, 0xE8));

	/* a hit without a prologue is dropped and does not disturb a valid one
	 * (in particular it must not pair with a zero qword before an OPD) */
	fake_reset_all();
	put_pattern(B + 0x100000, LD_TOC2, ADDI);
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* the nearest stdu wins: an OPD for the farther one is not enough */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put32(FUNC + 0x20, STDU);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());
	put_opd(OPD + 0x18, FUNC + 0x20, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));
	return 0;
}

static int test_scan_no_opd(void)
{
	/* prologue but no descriptor */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	EXPECT(expect_fail());

	/* descriptor whose toc is outside the window */
	put_opd(OPD, FUNC, 0x10000);
	EXPECT(expect_fail());
	put_opd(OPD, FUNC, END);
	EXPECT(expect_fail());

	/* a later descriptor with a toc inside the window is used */
	put_opd(OPD + 0x18, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* the entry must sit in an 8-aligned qword pair */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put64(OPD + 4, FUNC);
	put64(OPD + 12, TOC);
	EXPECT(expect_fail());

	/* the first valid pair wins */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	put_opd(OPD + 0x18, FUNC, TOC + 0x100);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));
	return 0;
}

static int test_scan_discards_invalid_hits(void)
{
	/* negative report offset */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI_NEG);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());
	/* ...is discarded, not counted: a valid hit elsewhere still wins */
	put_func(FUNC + 0x10000, FUNC + 0x10040, LD_TOC2, ADDI);
	put_opd(OPD + 0x18, FUNC + 0x10000, TOC);
	EXPECT(expect_result(FUNC + 0x10040, SLOT + 8, 0xE8));

	/* slot not 8-aligned */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOCU, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_fail());

	/* slot below the window */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD, FUNC, B + 0x10);
	EXPECT(expect_fail());

	/* slot past the window (TOC + 0x10 >= BOUND) */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOCP, ADDI);
	put_opd(OPD, FUNC, END - 8);
	EXPECT(expect_fail());
	/* last qword of the window is a valid slot */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOCP, ADDI);
	put_opd(OPD, FUNC, END - 0x18);
	EXPECT(expect_result(FUNC + 0x40, END - 8, 0xE8));
	return 0;
}

static int test_scan_hit_cap(void)
{
	int i;

	/* DS45_SCAN_MAX_HITS identical hits under one prologue: one result */
	fake_reset_all();
	put_func(FUNC, FUNC + 0x10, LD_TOC, ADDI);
	for (i = 1; i < DS45_SCAN_MAX_HITS; i++)
		put_pattern(FUNC + 0x10 + 0x10 * (uint64_t)i, LD_TOC, ADDI);
	put_opd(OPD, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x10, SLOT, 0xE8));

	/* one more overflows the hit table: ambiguous */
	put_pattern(FUNC + 0x10 + 0x10 * (uint64_t)DS45_SCAN_MAX_HITS, LD_TOC, ADDI);
	EXPECT(expect_fail());
	return 0;
}

static int test_scan_never_leaves_window(void)
{
	/* prologue at the first word of the window (backward search clamps at
	 * BASE), descriptor in the last two qwords of the window */
	fake_reset_all();
	put_func(B, B + 8, LD_TOC, ADDI);
	put64(END - 16, B);
	put64(END - 8, TOC);
	EXPECT(expect_result(B + 8, SLOT, 0xE8));

	/* hit at the very first word, no prologue anywhere */
	fake_reset_all();
	put_pattern(B, LD_TOC, ADDI);
	put_opd(OPD, B, TOC);
	EXPECT(expect_fail());

	/* pattern in the last three words of the window: its fingerprint
	 * (END - 0x10 .. END + 0x10) would leave the window, so it is dropped */
	fake_reset_all();
	put_func(END - 0x20, END - 0xC, LD_TOC, ADDI);
	put_opd(OPD, END - 0x20, TOC);
	EXPECT(expect_fail());
	/* ...and, dropped, it does not make an earlier valid hit ambiguous */
	put32(END - 0xC, LD_TOC2);
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);
	put_opd(OPD + 0x18, FUNC, TOC);
	EXPECT(expect_result(FUNC + 0x40, SLOT, 0xE8));

	/* fingerprint ending exactly at BOUND is fine */
	fake_reset_all();
	put_func(END - 0x40, END - 0x20, LD_TOC, ADDI);
	put_opd(OPD, END - 0x40, TOC);
	EXPECT(expect_result(END - 0x20, SLOT, 0xE8));
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_scan_empty_pattern(void)
{
	static const ds45_scan_pattern_t empty = { pat_insns, 0, 0, 0 };
	static const ds45_scan_pattern_t too_long = { pat_insns, DS45_SCAN_MAX_INSNS + 1, 0, 2 };
	static const ds45_scan_pattern_t bad_ld = { pat_insns, 3, 3, 2 };
	static const ds45_scan_pattern_t bad_off = { pat_insns, 3, 0, -1 };
	static const ds45_scan_pattern_t no_insns = { NULL, 3, 0, 2 };
	ds45_kernel_t out;

	fake_reset_all();
	put_func(FUNC, FUNC + 0x40, LD_TOC, ADDI);   /* a scannable image */
	put_opd(OPD, FUNC, TOC);

	CHECK(ds45_scan_pattern.count == 0);         /* real pattern: Task 11 */
	CHECK(kernel_scan(&ds45_scan_pattern, &out) == -1);
	CHECK(kernel_scan(&empty, &out) == -1);
	CHECK(kernel_scan(&too_long, &out) == -1);
	CHECK(kernel_scan(&bad_ld, &out) == -1);
	CHECK(kernel_scan(&bad_off, &out) == -1);
	CHECK(kernel_scan(&no_insns, &out) == -1);
	CHECK(kernel_scan(NULL, &out) == -1);
	CHECK(kernel_scan(&pat, NULL) == -1);
	CHECK(fake_lv2_peek_count == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

int run_tests(void)
{
	int r;
#define RUN(t) do { r = t(); if (r) return r; } while (0)
	RUN(test_scan_single_hit);
	RUN(test_scan_no_hit);
	RUN(test_scan_ambiguous);
	RUN(test_scan_duplicate_hits_same_result);
	RUN(test_scan_no_prologue);
	RUN(test_scan_no_opd);
	RUN(test_scan_discards_invalid_hits);
	RUN(test_scan_hit_cap);
	RUN(test_scan_never_leaves_window);
	RUN(test_scan_empty_pattern);
	return 0;
}
