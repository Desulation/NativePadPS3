/* SOURCES: lv2.c kpatch.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kpatch.h"

#define A        (DS45_LV2_BASE + 0x3000)   /* scratch area used by every test */
#define SNAP_LEN 0x80

/* Image layout (all big-endian, as on the PPU):
 *   A + 0x00  qword  pointer slot
 *   A + 0x08  qword  two instructions; high half at A+8, low half at A+12
 *   A + 0x10  qword  two instructions; high half at A+16, low half at A+20
 *   A + 0x18  qword  a second pointer slot
 */
#define PTR0     0x8000000000123456ULL
#define Q1       0x6000000038600001ULL
#define Q2       0x7C0802A6F8010010ULL
#define PTR3     0x80000000007FFF00ULL

static void put_q(uint64_t addr, uint64_t v)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	fake_lv2_set(addr, b, 8);
}

/* FNV-1a 64 over the 8 bytes of a qword in memory order. */
static uint64_t qhash(uint64_t v)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	return ds45_fnv1a64(b, 8);
}

static void fill_image(void)
{
	fake_reset_all();
	put_q(A + 0x00, PTR0);
	put_q(A + 0x08, Q1);
	put_q(A + 0x10, Q2);
	put_q(A + 0x18, PTR3);
}

static void snap(uint8_t *out)
{
	ds45_lv2_read_bytes(A, out, SNAP_LEN);
}

static uint64_t rd(uint64_t addr)
{
	uint64_t v = 0;

	ds45_lv2_read(addr, &v);
	return v;
}

static int test_verify_ptr_mismatch_writes_nothing(void)
{
	ds45_kpatch_t p[2];
	ds45_kjournal_t j;
	uint8_t before[SNAP_LEN], after[SNAP_LEN];
	int pokes;

	fill_image();
	snap(before);

	/* a PTR whose current value is not what the patch expects */
	p[0].kind = DS45_KPATCH_PTR;
	p[0].addr = A;
	p[0].expect = PTR0 + 8;
	p[0].value = 0x8000000000001000ULL;
	CHECK(kpatch_verify(p, 1) != 0);
	memset(&j, 0x55, sizeof(j));
	CHECK(kpatch_apply(p, 1, &j) == 1);
	CHECK(j.n == 0);
	CHECK(fake_lv2_poke_count == 0);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);

	/* the first patch is fine, the second is not: still nothing is written,
	 * because every target is checked before the first poke */
	p[0].expect = PTR0;
	p[1].kind = DS45_KPATCH_PTR;
	p[1].addr = A + 0x18;
	p[1].expect = PTR3 ^ 1;
	p[1].value = 0x8000000000002000ULL;
	CHECK(kpatch_verify(p, 2) == 2);
	CHECK(kpatch_apply(p, 2, &j) == 2);
	CHECK(j.n == 0);
	CHECK(fake_lv2_poke_count == 0);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);

	/* both fine: verify passes and, being read-only, writes nothing either */
	p[1].expect = PTR3;
	pokes = fake_lv2_poke_count;
	CHECK(kpatch_verify(p, 2) == 0);
	CHECK(fake_lv2_poke_count == pokes);
	CHECK(kpatch_verify(p, 0) == 0);

	/* WORD / BRANCH: a wrong hash fails, so does a hash of the wrong half */
	p[0].kind = DS45_KPATCH_WORD;
	p[0].addr = A + 12;
	p[0].expect = qhash(Q1) ^ 1;
	p[0].value = 0x4E800020ULL;
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].expect = qhash(Q1 & 0xFFFFFFFFULL);       /* hash of the wrong bytes */
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].expect = qhash(Q1);
	CHECK(kpatch_verify(p, 1) == 0);
	p[0].kind = DS45_KPATCH_BRANCH;
	p[0].addr = A + 16;
	p[0].expect = qhash(Q2) ^ 1;
	p[0].value = A + 0x400;
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].expect = qhash(Q2);
	CHECK(kpatch_verify(p, 1) == 0);

	/* a branch target out of range fails verification */
	p[0].value = A + 16 + 0x2000000ULL;            /* +32 MiB: one step too far */
	CHECK(kpatch_verify(p, 1) == 1);
	CHECK(kpatch_apply(p, 1, &j) == 1);
	CHECK(fake_lv2_poke_count == 0);

	/* alignment, window, kind and value-width violations */
	p[0].kind = DS45_KPATCH_PTR;
	p[0].addr = A + 4;                             /* PTR needs 8-aligned */
	p[0].expect = rd(A) ;
	p[0].value = 1;
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].kind = DS45_KPATCH_WORD;
	p[0].addr = A + 10;                            /* WORD needs 4-aligned */
	p[0].expect = qhash(Q1);
	p[0].value = 1;
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].addr = DS45_LV2_BOUND;                    /* outside the window */
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].addr = A + 12;
	p[0].value = 0x100000000ULL;                   /* does not fit in 32 bits */
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].value = 1;
	p[0].kind = (ds45_kpatch_kind_t)0;             /* unknown kind */
	CHECK(kpatch_verify(p, 1) == 1);
	p[0].kind = (ds45_kpatch_kind_t)99;
	CHECK(kpatch_verify(p, 1) == 1);
	CHECK(kpatch_verify(NULL, 1) == -1);
	CHECK(kpatch_apply(p, 1, NULL) == -1);

	CHECK(fake_lv2_poke_count == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_apply_ptr_and_word(void)
{
	ds45_kpatch_t p[3];
	ds45_kjournal_t j;
	uint32_t w = 0;

	fill_image();

	p[0].kind = DS45_KPATCH_PTR;
	p[0].addr = A;
	p[0].expect = PTR0;
	p[0].value = 0x8000000000ABCDE8ULL;
	/* WORD in the low half (addr & 4 != 0): the high half is kept */
	p[1].kind = DS45_KPATCH_WORD;
	p[1].addr = A + 12;
	p[1].expect = qhash(Q1);
	p[1].value = 0x4E800020ULL;

	CHECK(kpatch_apply(p, 2, &j) == 0);
	CHECK(rd(A) == 0x8000000000ABCDE8ULL);
	CHECK(rd(A + 8) == 0x600000004E800020ULL);
	CHECK(j.n == 2);
	CHECK(j.qaddr[0] == A && j.orig[0] == PTR0);
	CHECK(j.qaddr[1] == A + 8 && j.orig[1] == Q1);
	CHECK(fake_lv2_poke_count == 2);               /* one poke per qword */
	CHECK(kpatch_verify(p, 2) != 0);               /* no longer as expected */

	/* WORD in the high half (addr & 4 == 0): the low half is kept */
	p[0].kind = DS45_KPATCH_WORD;
	p[0].addr = A + 16;
	p[0].expect = qhash(Q2);
	p[0].value = 0x60000000ULL;
	CHECK(kpatch_apply(p, 1, &j) == 0);
	CHECK(rd(A + 16) == 0x60000000F8010010ULL);
	CHECK(j.n == 1 && j.qaddr[0] == A + 16 && j.orig[0] == Q2);

	/* BRANCH is encoded as `b target`, relative to the instruction itself */
	fill_image();
	CHECK(kpatch_branch_word(A + 12, A + 12 + 0x200, &w) == 0);
	CHECK(w == 0x48000200u);
	p[0].kind = DS45_KPATCH_BRANCH;
	p[0].addr = A + 12;
	p[0].expect = qhash(Q1);
	p[0].value = A + 12 + 0x200;
	CHECK(kpatch_apply(p, 1, &j) == 0);
	CHECK(rd(A + 8) == 0x6000000048000200ULL);
	CHECK(j.n == 1 && j.orig[0] == Q1);

	/* two patches in the same qword: the second sees the first one's result,
	 * and reverting both restores the original */
	fill_image();
	p[0].kind = DS45_KPATCH_WORD;
	p[0].addr = A + 8;
	p[0].expect = qhash(Q1);
	p[0].value = 0x11111111ULL;
	p[1].kind = DS45_KPATCH_WORD;
	p[1].addr = A + 12;
	p[1].expect = qhash(Q1);
	p[1].value = 0x22222222ULL;
	CHECK(kpatch_apply(p, 2, &j) == 0);
	CHECK(rd(A + 8) == 0x1111111122222222ULL);
	CHECK(j.n == 2 && j.orig[0] == Q1 && j.orig[1] == 0x1111111138600001ULL);
	kpatch_revert(&j);
	CHECK(rd(A + 8) == Q1);
	return 0;
}

static int test_branch_encoding(void)
{
	uint32_t w = 0xDEADBEEF;
	uint64_t f = DS45_LV2_BASE + 0x1000;

	CHECK(kpatch_branch_word(f, DS45_LV2_BASE + 0x2000, &w) == 0);
	CHECK(w == 0x48001000u);

	/* backwards: the displacement is two's complement in the 24-bit LI field */
	CHECK(kpatch_branch_word(DS45_LV2_BASE + 0x2000, f, &w) == 0);
	CHECK(w == 0x4BFFF000u);
	CHECK(kpatch_branch_word(f, f - 4, &w) == 0);
	CHECK(w == 0x4BFFFFFCu);
	CHECK(kpatch_branch_word(f, f, &w) == 0);       /* branch to self */
	CHECK(w == 0x48000000u);

	/* the limits: -32 MiB .. +32 MiB - 4 encode, one more word does not */
	CHECK(kpatch_branch_word(f, f + 0x1FFFFFCULL, &w) == 0);
	CHECK(w == 0x49FFFFFCu);
	CHECK(kpatch_branch_word(f + 0x2000000ULL, f, &w) == 0);
	CHECK(w == 0x4A000000u);

	w = 0xDEADBEEF;
	CHECK(kpatch_branch_word(f, f + 0x2000000ULL, &w) == -1);
	CHECK(w == 0xDEADBEEFu);                        /* untouched on failure */
	CHECK(kpatch_branch_word(f + 0x2000004ULL, f, &w) == -1);
	CHECK(w == 0xDEADBEEFu);

	/* a displacement that is not a multiple of 4 cannot be encoded */
	CHECK(kpatch_branch_word(f, f + 2, &w) == -1);
	CHECK(kpatch_branch_word(f, f + 1, &w) == -1);
	CHECK(w == 0xDEADBEEFu);
	CHECK(kpatch_branch_word(f, f + 8, NULL) == -1);

	/* LK and AA stay clear: the low two bits are always zero */
	CHECK(kpatch_branch_word(f, f + 0x100, &w) == 0);
	CHECK((w & 3u) == 0);
	CHECK((w >> 26) == 18u);
	return 0;
}

/* A transient fault: the fake drops every poke once fake_lv2_poke_fail_after
 * pokes have landed, which would also swallow the reverts. Here the fault ends
 * as soon as the first dropped poke has been read back, so the reverts that
 * follow can land. */
static int g_fault_at;

static void clear_fault_after_failed_poke(uint64_t addr)
{
	(void)addr;
	if (fake_lv2_poke_count > g_fault_at)
		fake_lv2_poke_fail_after = -1;
}

static void arm_fault(int after)
{
	g_fault_at = after;
	fake_lv2_poke_fail_after = after;
	fake_lv2_on_peek = clear_fault_after_failed_poke;
}

static int test_apply_rollback_on_readback_failure(void)
{
	ds45_kpatch_t p[3];
	ds45_kjournal_t j;
	uint8_t before[SNAP_LEN], after[SNAP_LEN];

	fill_image();
	snap(before);

	p[0].kind = DS45_KPATCH_PTR;
	p[0].addr = A;
	p[0].expect = PTR0;
	p[0].value = 0x8000000000ABCDE8ULL;
	p[1].kind = DS45_KPATCH_WORD;
	p[1].addr = A + 12;
	p[1].expect = qhash(Q1);
	p[1].value = 0x4E800020ULL;
	p[2].kind = DS45_KPATCH_PTR;
	p[2].addr = A + 0x18;
	p[2].expect = PTR3;
	p[2].value = 0x8000000000ABCDF0ULL;

	/* one poke lands, the second is dropped (its read-back fails) */
	arm_fault(1);
	CHECK(kpatch_apply(p, 3, &j) == 2);
	fake_lv2_on_peek = NULL;
	CHECK(j.n == 0);                                /* journal consumed */
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);    /* byte-identical */
	CHECK(rd(A) == PTR0);                           /* patch 1 was undone */
	CHECK(rd(A + 0x18) == PTR3);                    /* patch 3 never ran */
	CHECK(fake_lv2_out_of_bounds == 0);

	/* the very first write fails: nothing to undo but the image is intact */
	fill_image();
	arm_fault(0);
	CHECK(kpatch_apply(p, 3, &j) == 1);
	fake_lv2_on_peek = NULL;
	CHECK(j.n == 0);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);

	/* the last write fails: both earlier ones are undone */
	fill_image();
	arm_fault(2);
	CHECK(kpatch_apply(p, 3, &j) == 3);
	fake_lv2_on_peek = NULL;
	CHECK(j.n == 0);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);
	CHECK(rd(A) == PTR0 && rd(A + 8) == Q1);

	/* same patches, no fault: they apply cleanly (the rollbacks above were
	 * not an artefact of broken patches) */
	fill_image();
	CHECK(kpatch_apply(p, 3, &j) == 0);
	CHECK(j.n == 3);
	return 0;
}

static int test_revert_restores_all(void)
{
	ds45_kpatch_t p[3];
	ds45_kjournal_t j;
	uint8_t before[SNAP_LEN], mid[SNAP_LEN], after[SNAP_LEN];
	int pokes;

	fill_image();
	snap(before);

	p[0].kind = DS45_KPATCH_PTR;
	p[0].addr = A;
	p[0].expect = PTR0;
	p[0].value = 0x8000000000ABCDE8ULL;
	p[1].kind = DS45_KPATCH_WORD;
	p[1].addr = A + 12;
	p[1].expect = qhash(Q1);
	p[1].value = 0x4E800020ULL;
	p[2].kind = DS45_KPATCH_BRANCH;
	p[2].addr = A + 16;
	p[2].expect = qhash(Q2);
	p[2].value = A + 0x800;

	CHECK(kpatch_apply(p, 3, &j) == 0);
	CHECK(j.n == 3);
	snap(mid);
	CHECK(memcmp(before, mid, SNAP_LEN) != 0);

	pokes = fake_lv2_poke_count;
	kpatch_revert(&j);
	CHECK(j.n == 0);
	CHECK(fake_lv2_poke_count == pokes + 3);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);

	/* an empty journal is a no-op, so a second revert touches nothing */
	pokes = fake_lv2_poke_count;
	kpatch_revert(&j);
	CHECK(j.n == 0);
	CHECK(fake_lv2_poke_count == pokes);
	kpatch_revert(NULL);                            /* must not crash */

	/* reverting is last-first: with two entries for the same qword the
	 * earliest original is what remains */
	put_q(A, 0x1111111111111111ULL);
	j.n = 2;
	j.qaddr[0] = A; j.orig[0] = 0xAAAAAAAAAAAAAAAAULL;
	j.qaddr[1] = A; j.orig[1] = 0xBBBBBBBBBBBBBBBBULL;
	kpatch_revert(&j);
	CHECK(rd(A) == 0xAAAAAAAAAAAAAAAAULL);
	CHECK(j.n == 0);

	/* a failed read-back during revert does not stop the remaining entries */
	fill_image();
	j.n = 3;
	j.qaddr[0] = A;        j.orig[0] = 0x0101010101010101ULL;
	j.qaddr[1] = A + 8;    j.orig[1] = 0x0202020202020202ULL;
	j.qaddr[2] = A + 0x10; j.orig[2] = 0x0303030303030303ULL;
	fake_lv2_poke_fail_after = 0;                   /* every revert write is dropped */
	pokes = fake_lv2_poke_count;
	kpatch_revert(&j);
	CHECK(fake_lv2_poke_count == pokes + 3);        /* all three were attempted */
	CHECK(j.n == 0);
	CHECK(rd(A) == PTR0);                           /* nothing stuck */

	/* a corrupt journal never turns into an out-of-window write */
	fill_image();
	j.n = 2;
	j.qaddr[0] = A;                 j.orig[0] = 0x0101010101010101ULL;
	j.qaddr[1] = DS45_LV2_BOUND;    j.orig[1] = 1;
	fake_lv2_poke_fail_after = -1;
	kpatch_revert(&j);
	CHECK(fake_lv2_out_of_bounds == 0);
	CHECK(rd(A) == 0x0101010101010101ULL);          /* the valid entry still reverted */
	CHECK(j.n == 0);
	return 0;
}

static int test_too_many_patches(void)
{
	ds45_kpatch_t p[DS45_KPATCH_MAX + 1];
	ds45_kjournal_t j;
	uint8_t before[SNAP_LEN], after[SNAP_LEN];
	int i;

	fill_image();
	snap(before);

	for (i = 0; i <= DS45_KPATCH_MAX; i++) {
		p[i].kind = DS45_KPATCH_PTR;
		p[i].addr = A + 0x20 + 8 * (uint64_t)i;
		p[i].expect = 0;
		p[i].value = 0x8000000000001000ULL + (uint64_t)i;
	}

	fake_lv2_peek_count = 0;
	memset(&j, 0x55, sizeof(j));
	CHECK(kpatch_apply(p, DS45_KPATCH_MAX + 1, &j) == -1);
	CHECK(kpatch_apply(p, -1, &j) == -1);
	CHECK(kpatch_verify(p, DS45_KPATCH_MAX + 1) == -1);
	CHECK(kpatch_verify(p, -1) == -1);
	CHECK(fake_lv2_peek_count == 0);                /* no reads at all */
	CHECK(fake_lv2_poke_count == 0);
	snap(after);
	CHECK(memcmp(before, after, SNAP_LEN) == 0);

	/* zero patches: success, empty journal */
	memset(&j, 0x55, sizeof(j));
	CHECK(kpatch_apply(p, 0, &j) == 0);
	CHECK(j.n == 0);

	/* exactly DS45_KPATCH_MAX is allowed and journals every one */
	CHECK(kpatch_apply(p, DS45_KPATCH_MAX, &j) == 0);
	CHECK(j.n == DS45_KPATCH_MAX);
	for (i = 0; i < DS45_KPATCH_MAX; i++)
		CHECK(rd(p[i].addr) == p[i].value);
	kpatch_revert(&j);
	for (i = 0; i < DS45_KPATCH_MAX; i++)
		CHECK(rd(p[i].addr) == 0);
	return 0;
}

int run_tests(void)
{
	int line;

	line = test_verify_ptr_mismatch_writes_nothing();
	if (line)
		return line;
	line = test_apply_ptr_and_word();
	if (line)
		return line;
	line = test_branch_encoding();
	if (line)
		return line;
	line = test_apply_rollback_on_readback_failure();
	if (line)
		return line;
	line = test_revert_restores_all();
	if (line)
		return line;
	line = test_too_many_patches();
	if (line)
		return line;

	return 0;
}
