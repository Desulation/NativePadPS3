/* SOURCES: lv2.c kpatch.c kshim.c kshim_table.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "fakes.h"
#include "lv2.h"
#include "kpatch.h"
#include "kernel.h"      /* DS45_FP_LEN; kernel.c is not linked, so we supply
                          * kernel_fingerprint_matches below (a copy of it). */
#include "kshim.h"
#include "../shim/shim_abi.h"
#include "shim_blob.h"
#include "ds45pad.h"     /* ds45_config_t g_cfg */

/* Synthetic image only: made-up fingerprint bytes and in-window addresses, no
 * kernel bytes. The one real input is src/shim_blob.h (the freshly packed blob
 * the VSH build generates), so the relocation and data-block checks run against
 * the exact bytes the installer copies. */

#define B DS45_LV2_BASE

/* Table-entry addresses (all in-window, chosen clear of one another and of the
 * blob region). */
#define FP_ADDR     (B + 0x1000)
#define SYS_TABLE   (B + 0x2000)
#define SPARE_NUM   149u
#define SPARE_SLOT  (SYS_TABLE + (uint64_t)SPARE_NUM * 8)   /* B + 0x24A8 */
#define SPARE_STUB  0x80000000003621B0ULL                  /* ni stub OPD */
#define ALLOC_DESC  (B + 0x3000)
#define ALLOC_ENTRY 0x80000000000681F4ULL
#define KERNEL_TOC  0x80000000003759C0ULL
#define ALLOC_POOL  0x22ULL
#define FLUSH_ENTRY 0x80000000000029B8ULL
#define STARTUP_SLOT (B + 0x6000)
#define STARTUP_ORIG 0x800000000033A208ULL
#define INPUT_SLOT   (B + 0x6100)
#define INPUT_ORIG   0x800000000033A010ULL
/* The kernel's BT-HID transfer routine as a row carries it: { entry, toc }. The
 * values are distinct from every other address here and from KERNEL_TOC, so the
 * test sees the installer take the row's toc rather than the allocator's. */
#define SEND_ENTRY   0x8000000000403000ULL
#define SEND_TOC     0x8000000000403800ULL

/* The three F4 product-ID sites, shaped like the DEX 4.92 row: the patched
 * instruction is the LOW half of its qword (addr & 4 set) and the high half is a
 * neighbouring instruction that has to survive. Two WORD rows (the inline compare
 * and the inline fall-through branch) carry their own finished instruction; one
 * BRANCH row (the helper's first instruction) becomes `b shim_pid_ok`. The
 * original qwords are made-up patterns, not kernel bytes. */
#define PID0_QA      (B + 0x7000)          /* WORD: the qword's address ... */
#define PID0_SITE    (PID0_QA + 4)         /* ... and the instruction in it */
#define PID0_Q       0x1A1A1A1A2B2B2B2BULL
#define PID0_WORD    0x2F800CE6u           /* the table's value: never base-adjusted */
#define PID1_QA      (B + 0x7008)          /* WORD */
#define PID1_SITE    (PID1_QA + 4)
#define PID1_Q       0x3C3C3C3C4D4D4D4DULL
#define PID1_WORD    0x4BFFFC48u
#define PID2_QA      (B + 0x8000)          /* BRANCH */
#define PID2_SITE    (PID2_QA + 4)
#define PID2_Q       0x5E5E5E5E6F6F6F6FULL
#define HI32         0xFFFFFFFF00000000ULL

/* The address the fake allocator hands back: in the 0x6xxxxx heap region F6
 * says pool-0x22 lands in, and clear of every table-entry address above. */
#define BLOB_BASE    (B + 0x600000)
/* An allocation the installer must reject: past the 8 MiB window, hence also
 * past a 26-bit branch of the F4 sites (the window is 8 MiB, a branch reaches
 * +/-32 MiB, so an in-window allocation is always in branch range - only an
 * out-of-window one can be out of range). */
#define BLOB_BAD     (B + 0x4000000ULL)

#define TOCTOU_VAL   0x8000000000BAD008ULL   /* a "concurrent" slot change */

#define CHECK_SUB(e) do { int l_ = (e); if (l_) return l_; } while (0)

/* ----- byte/qword helpers (big-endian, as on the PPU) -------------------- */

static void put_q(uint64_t addr, uint64_t v)
{
	uint8_t b[8];
	int i;
	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	fake_lv2_set(addr, b, 8);
}

static uint64_t rd(uint64_t addr)
{
	uint64_t v = 0;
	ds45_lv2_read(addr, &v);
	return v;
}

static uint64_t qhash(uint64_t v)
{
	uint8_t b[8];
	int i;
	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	return ds45_fnv1a64(b, 8);
}

/* The blob's own bytes at offset off, assembled as a big-endian qword. */
static uint64_t blob_q(uint32_t off)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v = (v << 8) | ds45_shim_blob[off + i];
	return v;
}

static int is_reloc(uint32_t off)
{
	int i;
	for (i = 0; i < DS45_SHIM_RELOC_COUNT; i++)
		if (ds45_shim_relocs[i] == off)
			return 1;
	return 0;
}

static uint64_t be64b(const uint8_t *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}
static uint32_t be32b(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t be16b(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* ----- kernel.c symbol kshim.c needs (kernel.c is not in SOURCES) -------- */

int kernel_fingerprint_matches(uint64_t fp_addr, uint64_t fp_hash)
{
	uint8_t buf[DS45_FP_LEN];
	if (fp_addr == 0)
		return 0;
	if (ds45_lv2_read_bytes(fp_addr, buf, DS45_FP_LEN) != 0)
		return 0;
	return ds45_fnv1a64(buf, DS45_FP_LEN) == fp_hash;
}

/* g_cfg lives in config.c, which is not linked here. */
ds45_config_t g_cfg;

/* ----- the spare-syscall stand-in ---------------------------------------- */

static int      g_spare_calls;
static uint64_t g_spare_a1[4], g_spare_a2[4];
static uint64_t g_alloc_ret;    /* what the first (allocator) call returns */

static uint64_t spare_handler(uint64_t a1, uint64_t a2)
{
	if (g_spare_calls < 4) {
		g_spare_a1[g_spare_calls] = a1;
		g_spare_a2[g_spare_calls] = a2;
	}
	g_spare_calls++;
	/* Call 1 is the allocation (returns the base); any later call is the
	 * cache flush, whose result the installer ignores. */
	return g_spare_calls == 1 ? g_alloc_ret : 0;
}

/* ----- image + table setup ----------------------------------------------- */

static ds45_shim_kernel_t g_ent;

static void setup(void)
{
	uint8_t fp[DS45_FP_LEN];
	int i;

	fake_reset_all();
	g_spare_calls = 0;
	memset(g_spare_a1, 0, sizeof(g_spare_a1));
	memset(g_spare_a2, 0, sizeof(g_spare_a2));
	g_alloc_ret = BLOB_BASE;
	fake_spare_syscall = (int)SPARE_NUM;
	fake_spare_handler = spare_handler;

	/* distinctive, out-of-default config so the cfg copy is observable */
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.stick_deadzone = 12;
	g_cfg.map_touchpad = (ds45_map_t)3;
	g_cfg.map_mute = (ds45_map_t)5;
	g_cfg.sixaxis = 1;
	g_cfg.swap_xy = 1;
	g_cfg.invert_x = 1;
	g_cfg.invert_y = 0;
	g_cfg.invert_z = 1;
	g_cfg.invert_g = 0;
	g_cfg.accel_scale = 150;
	g_cfg.gyro_scale = -50;
	/* the shim is gated off by default (ENABLE_SHIM); every install test opts in */
	g_cfg.enable_shim = 1;

	/* fingerprint */
	for (i = 0; i < DS45_FP_LEN; i++)
		fp[i] = (uint8_t)(0x40 + i * 7);
	fake_lv2_set(FP_ADDR, fp, DS45_FP_LEN);

	/* slots and the allocator OPD */
	put_q(SPARE_SLOT, SPARE_STUB);
	put_q(ALLOC_DESC, ALLOC_ENTRY);
	put_q(ALLOC_DESC + 8, KERNEL_TOC);
	put_q(STARTUP_SLOT, STARTUP_ORIG);
	put_q(INPUT_SLOT, INPUT_ORIG);
	put_q(PID0_QA, PID0_Q);
	put_q(PID1_QA, PID1_Q);
	put_q(PID2_QA, PID2_Q);

	/* synthetic table row */
	memset(&g_ent, 0, sizeof(g_ent));
	g_ent.name = "SYN";
	g_ent.fp_addr = FP_ADDR;
	g_ent.fp_hash = ds45_fnv1a64(fp, DS45_FP_LEN);
	g_ent.syscall_table = SYS_TABLE;
	g_ent.spare_syscall = SPARE_NUM;
	g_ent.spare_slot_expect = SPARE_STUB;
	g_ent.alloc_desc = ALLOC_DESC;
	g_ent.alloc_pool = ALLOC_POOL;
	g_ent.flush_desc = FLUSH_ENTRY;
	g_ent.startup_slot = STARTUP_SLOT;
	g_ent.startup_expect = STARTUP_ORIG;
	g_ent.input_slot = INPUT_SLOT;
	g_ent.input_expect = INPUT_ORIG;
	g_ent.send_entry = SEND_ENTRY;
	g_ent.send_toc = SEND_TOC;
	g_ent.pid_patches[0].kind = DS45_KPATCH_WORD;
	g_ent.pid_patches[0].addr = PID0_SITE;
	g_ent.pid_patches[0].expect = qhash(PID0_Q);
	g_ent.pid_patches[0].value = PID0_WORD;
	g_ent.pid_patches[1].kind = DS45_KPATCH_WORD;
	g_ent.pid_patches[1].addr = PID1_SITE;
	g_ent.pid_patches[1].expect = qhash(PID1_Q);
	g_ent.pid_patches[1].value = PID1_WORD;
	g_ent.pid_patches[2].kind = DS45_KPATCH_BRANCH;
	g_ent.pid_patches[2].addr = PID2_SITE;
	g_ent.pid_patches[2].expect = qhash(PID2_Q);
	g_ent.pid_patches[2].value = 0;
}

/* ----- tests ------------------------------------------------------------- */

/* The installer writes sizeof(ds45_shim_data_t) bytes at OFF_DATA, and the flush
 * OPD goes at BLOB_LEN: the data block must be exactly the blob's tail. A stale
 * generated src/shim_blob.h (built before the data block last changed) breaks
 * that and would corrupt the OPD - regenerate it with `src/build.ps1 -Variant
 * vsh`. Run first so a stale header says so rather than failing obscurely. */
static int test_blob_tail_is_the_data_block(void)
{
	CHECK(DS45_SHIM_OFF_DATA + sizeof(ds45_shim_data_t) == DS45_SHIM_BLOB_LEN);
	return 0;
}

static int test_no_entry_writes_nothing(void)
{
	uint8_t fp[DS45_FP_LEN];
	int i, pokes;

	setup();
	/* corrupt the fingerprint so no row matches */
	for (i = 0; i < DS45_FP_LEN; i++)
		fp[i] = (uint8_t)(0x99 + i);
	fake_lv2_set(FP_ADDR, fp, DS45_FP_LEN);

	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_NO_ENTRY);
	CHECK(fake_lv2_poke_count == pokes);   /* nothing written */
	CHECK(g_spare_calls == 0);             /* nothing allocated */

	/* an empty / NULL table is also NO_ENTRY, no reads needed */
	CHECK(kshim_install_from(NULL, 0) == KSHIM_NO_ENTRY);
	CHECK(kshim_install_from(&g_ent, 0) == KSHIM_NO_ENTRY);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* Verify the installed blob image: relocated words rebased, every other word
 * copied verbatim, the data block filled, and the flush OPD built. */
static int check_blob_image(void)
{
	uint8_t db[304];                 /* sizeof(ds45_shim_data_t), 8-aligned */
	const uint8_t *cfg;
	uint32_t off;
	int i;

	for (off = 0; off < DS45_SHIM_OFF_DATA; off += 8) {
		uint64_t want = blob_q(off);
		if (is_reloc(off))
			want += BLOB_BASE;
		CHECK_SUB(rd(BLOB_BASE + off) == want ? 0 : __LINE__);
	}

	CHECK_SUB(sizeof(ds45_shim_data_t) == sizeof db ? 0 : __LINE__);
	CHECK_SUB(ds45_lv2_read_bytes(BLOB_BASE + DS45_SHIM_OFF_DATA, db,
	                              (int)sizeof db) == 0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, magic)) ==
	          DS45_SHIM_MAGIC ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, version)) ==
	          DS45_SHIM_VERSION ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, enabled)) == 1 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_startup)) ==
	          STARTUP_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_input)) ==
	          INPUT_ORIG ? 0 : __LINE__);
	/* the kernel's transfer routine has no descriptor of its own: the data block
	 * carries one { entry, toc, env } built from the row, and send_routine is the
	 * address of that descriptor as loaded - the shim calls through it */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_opd)) ==
	          SEND_ENTRY ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_opd) + 8) ==
	          SEND_TOC ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_opd) + 16) ==
	          0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_routine)) ==
	          BLOB_BASE + DS45_SHIM_OFF_DATA +
	          offsetof(ds45_shim_data_t, send_opd) ? 0 : __LINE__);

	cfg = db + offsetof(ds45_shim_data_t, cfg);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, deadzone)] == 12 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, map_touchpad)] == 3 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, map_mute)] == 5 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, sixaxis)] == 1 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, swap_xy)] == 1 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, invert_x)] == 1 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, invert_y)] == 0 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, invert_z)] == 1 ? 0 : __LINE__);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, invert_g)] == 0 ? 0 : __LINE__);
	CHECK_SUB((int16_t)be16b(cfg + offsetof(ds45_shim_cfg_t, accel_scale)) ==
	          150 ? 0 : __LINE__);
	CHECK_SUB((int16_t)be16b(cfg + offsetof(ds45_shim_cfg_t, gyro_scale)) ==
	          -50 ? 0 : __LINE__);
	/* per-connection, request-buffer and counter fields start cleared */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, conn_key)) == 0 ? 0 : __LINE__);
	for (i = 0; i < DS45_SHIM_CONNS; i++) {
		CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, conn_id) + 8 * i) ==
		          0 ? 0 : __LINE__);
		CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, start_tries) + 8 * i) ==
		          0 ? 0 : __LINE__);
	}
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, conn_state)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pdu)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_translated)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_startups)) == 0 ? 0 : __LINE__);

	/* flush OPD placed just past the blob: { entry, kernel TOC } */
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN) == FLUSH_ENTRY ? 0 : __LINE__);
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN + 8) == KERNEL_TOC ? 0 : __LINE__);
	return 0;
}

static int test_install_happy_path(void)
{
	uint32_t bw = 0;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* redirects now point at the blob's own descriptors */
	CHECK(rd(STARTUP_SLOT) == BLOB_BASE + DS45_SHIM_OFF_STARTUP);
	CHECK(rd(INPUT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_INPUT);

	/* the two WORD rows write their own finished instruction into the low half of
	 * their qword - verbatim: not a branch, not the blob's address - and the high
	 * half (the neighbouring instruction) is kept */
	CHECK(rd(PID0_QA) == ((PID0_Q & HI32) | PID0_WORD));
	CHECK(rd(PID1_QA) == ((PID1_Q & HI32) | PID1_WORD));

	/* the BRANCH row becomes `b shim_pid_ok` (low half), high half kept */
	CHECK(kpatch_branch_word(PID2_SITE, BLOB_BASE + DS45_SHIM_OFF_PID_OK, &bw) == 0);
	CHECK(rd(PID2_QA) == ((PID2_Q & HI32) | bw));

	/* the spare slot is back to the ni stub */
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);

	/* the blob, data block and flush OPD are correct */
	CHECK(check_blob_image() == 0);

	/* the allocator and the flush both went through the spare slot */
	CHECK(g_spare_calls == 2);
	CHECK(g_spare_a1[0] == DS45_SHIM_BLOB_LEN + 16);   /* alloc size */
	CHECK(g_spare_a2[0] == ALLOC_POOL);
	CHECK(g_spare_a1[1] == BLOB_BASE);                 /* flush addr */
	CHECK(g_spare_a2[1] == DS45_SHIM_BLOB_LEN);        /* flush len */

	CHECK(fake_lv2_out_of_bounds == 0);
	kshim_remove();
	return 0;
}

/* Snapshot of everything the installer is responsible for leaving untouched on
 * failure (the freshly-allocated blob region is scratch and excluded - on a
 * hard poke fault the installer cannot rewrite it, and it is not part of the
 * pre-install kernel image). The spare slot is explicitly included. */
struct snap { uint64_t startup, input, spare, pid0, pid1, pid2; };

static void take_snap(struct snap *s)
{
	s->startup = rd(STARTUP_SLOT);
	s->input   = rd(INPUT_SLOT);
	s->spare   = rd(SPARE_SLOT);
	s->pid0    = rd(PID0_QA);
	s->pid1    = rd(PID1_QA);
	s->pid2    = rd(PID2_QA);
}

static int snap_eq(const struct snap *a, const struct snap *b)
{
	return a->startup == b->startup && a->input == b->input &&
	       a->spare == b->spare && a->pid0 == b->pid0 && a->pid1 == b->pid1 &&
	       a->pid2 == b->pid2;
}

/* Where to start dropping pokes so the blob copy is cut off partway. The
 * installer's first two pokes borrow and restore the spare slot, so the blob's
 * qword at offset o is poke 3 + o/8 and `fail_after = 2 + o/8` drops exactly it.
 * A dropped poke only shows up if the qword it should have written differs from
 * what was there (a fresh, zeroed region), so o is the first qword at or after
 * 464 whose written value - relocated words get the load base added - is
 * non-zero, whatever the blob happens to contain between functions. -1 if none. */
static int blob_fault_point(void)
{
	uint32_t off;

	for (off = 464; off < DS45_SHIM_OFF_DATA; off += 8) {
		uint64_t q = blob_q(off);

		if (is_reloc(off))
			q += BLOB_BASE;
		if (q != 0)
			return 2 + (int)(off / 8);
	}
	return -1;
}

/* The transient-fault helper from test_kpatch: drop pokes from fail_after on,
 * then lift the fault as soon as the first dropped poke has been read back, so
 * the installer's own rollback writes can still land. */
static int g_fault_at;
static void clear_fault_after_failed_poke(uint64_t addr)
{
	(void)addr;
	if (fake_lv2_poke_count > g_fault_at)
		fake_lv2_poke_fail_after = -1;
}

/* case 4: a redirect slot changes between the verify and the write. */
static void toctou_on_peek(uint64_t addr)
{
	if (addr == INPUT_SLOT)
		put_q(INPUT_SLOT, TOCTOU_VAL);   /* a concurrent kernel write */
}

static int test_install_fails_each_step(void)
{
	struct snap before, after;

	/* (a) allocation returns 0 */
	setup();
	take_snap(&before);
	g_alloc_ret = 0;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(g_spare_calls == 1);             /* allocated, never flushed */
	take_snap(&after);
	CHECK(snap_eq(&before, &after));
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);

	/* (b) allocation out of range for the F4 branches (and the window) */
	setup();
	take_snap(&before);
	g_alloc_ret = BLOB_BAD;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(g_spare_calls == 1);             /* no flush: rejected before the copy */
	take_snap(&after);
	CHECK(snap_eq(&before, &after));
	/* nothing was written into the (bad) region either */
	CHECK(fake_lv2_out_of_bounds == 0);

	/* (c) a blob write does not read back (fault mid-copy) */
	setup();
	take_snap(&before);
	g_fault_at = blob_fault_point();       /* well into the blob copy */
	CHECK(g_fault_at > 0);
	fake_lv2_poke_fail_after = g_fault_at;
	fake_lv2_on_peek = clear_fault_after_failed_poke;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	fake_lv2_on_peek = NULL;
	take_snap(&after);
	CHECK(snap_eq(&before, &after));       /* every tracked target intact */
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);

	/* (d) the input redirect slot changes after it was verified */
	setup();
	take_snap(&before);
	fake_lv2_on_peek = toctou_on_peek;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	fake_lv2_on_peek = NULL;
	/* the installer refused to clobber the concurrently-changed slot */
	CHECK(rd(INPUT_SLOT) == TOCTOU_VAL);
	CHECK(rd(INPUT_SLOT) != BLOB_BASE + DS45_SHIM_OFF_INPUT);
	/* everything else is back to its original value */
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	CHECK(rd(PID0_QA) == before.pid0);
	CHECK(rd(PID1_QA) == before.pid1);
	CHECK(rd(PID2_QA) == before.pid2);
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	/* restore the test's artefact, then the whole set is byte-identical */
	put_q(INPUT_SLOT, INPUT_ORIG);
	take_snap(&after);
	CHECK(snap_eq(&before, &after));

	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* Step 2 checks every product-ID site, not just the first two: a kernel whose
 * third site is not the qword the row was researched against is refused before
 * anything is allocated or written. */
static int test_install_refuses_each_unexpected_pid_site(void)
{
	static const uint64_t qa[3] = { PID0_QA, PID1_QA, PID2_QA };
	const uint64_t other = 0xDEADBEEFDEADBEEFULL;    /* not what the row hashes */
	int i, pokes;

	for (i = 0; i < 3; i++) {
		setup();
		put_q(qa[i], other);
		pokes = fake_lv2_poke_count;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
		CHECK(fake_lv2_poke_count == pokes);         /* nothing written */
		CHECK(g_spare_calls == 0);                   /* nothing allocated */
		CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
		CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
		CHECK(rd(SPARE_SLOT) == SPARE_STUB);
		CHECK(rd(qa[i]) == other);                   /* left exactly as found */
	}
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A WORD row is a finished instruction: it lands verbatim whatever address the
 * blob was given. Only the BRANCH row follows the load base. (Setting a WORD
 * row's value to the blob address would not even encode: the value is 32 bits.) */
static int test_word_rows_do_not_follow_the_load_base(void)
{
	static const uint64_t base[2] = { BLOB_BASE, BLOB_BASE + 0x40000 };
	uint64_t w0[2], w1[2], br[2];
	uint32_t bw = 0;
	int i;

	for (i = 0; i < 2; i++) {
		setup();
		g_alloc_ret = base[i];
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		w0[i] = rd(PID0_QA);
		w1[i] = rd(PID1_QA);
		br[i] = rd(PID2_QA);
		CHECK(kpatch_branch_word(PID2_SITE, base[i] + DS45_SHIM_OFF_PID_OK, &bw) == 0);
		CHECK(br[i] == ((PID2_Q & HI32) | bw));      /* the branch targets this base */
		kshim_remove();
	}
	CHECK(w0[0] == ((PID0_Q & HI32) | PID0_WORD) && w0[1] == w0[0]);
	CHECK(w1[0] == ((PID1_Q & HI32) | PID1_WORD) && w1[1] == w1[0]);
	CHECK(br[0] != br[1]);                           /* ... and moves with it */
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* The final apply is one transaction of five patches - the three product-ID
 * patches (two WORD, one BRANCH) and the two redirects. A write part-way that does
 * not read back undoes the ones already made, whichever it is, and the kernel
 * image ends byte-identical to how it was found. */
static int test_install_rolls_back_a_partial_patch(void)
{
	struct snap before, after;
	int pokes, apply_start, i;

	/* calibrate on a clean install: remove writes back one qword per journal entry,
	 * and the entries are the last writes the installer made */
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	pokes = fake_lv2_poke_count;
	kshim_remove();
	CHECK(fake_lv2_poke_count - pokes == 5);
	apply_start = pokes - 5;

	for (i = 0; i < 5; i++) {
		setup();
		take_snap(&before);
		g_fault_at = apply_start + i;                /* drop the i-th patch write */
		fake_lv2_poke_fail_after = g_fault_at;
		fake_lv2_on_peek = clear_fault_after_failed_poke;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
		fake_lv2_on_peek = NULL;
		fake_lv2_poke_fail_after = -1;
		take_snap(&after);
		CHECK(snap_eq(&before, &after));             /* every patch undone */
		CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	}
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* The ENABLE_SHIM gate. With the flag off - its default - the installer does
 * nothing at all: no read (not even the fingerprint), no write, no allocation,
 * nothing recorded, even on a kernel whose row matches and would install. It runs
 * only once the config turns it on. */
static int test_install_is_gated_by_enable_shim(void)
{
	int peeks, pokes;

	setup();
	g_cfg.enable_shim = 0;
	peeks = fake_lv2_peek_count;
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_OFF);
	CHECK(fake_lv2_peek_count == peeks);             /* the kernel was not touched */
	CHECK(fake_lv2_poke_count == pokes);
	CHECK(g_spare_calls == 0);                       /* the spare syscall was not borrowed */
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q && rd(PID2_QA) == PID2_Q);
	CHECK(kshim_set_connection(0x2A, 0x1234) == -1); /* nothing is installed */
	kshim_remove();                                  /* harmless */
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);

	/* the same row and the same image install as soon as the flag is set */
	g_cfg.enable_shim = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(STARTUP_SLOT) == BLOB_BASE + DS45_SHIM_OFF_STARTUP);
	kshim_remove();

	/* kshim_install(), the built-in-table entry point the plugin's start-up calls,
	 * is gated the same way: off, it reads nothing; on, it goes looking for its row
	 * (the synthetic image matches none of the built-in fingerprints) */
	g_cfg.enable_shim = 0;
	peeks = fake_lv2_peek_count;
	CHECK(kshim_install() == KSHIM_OFF);
	CHECK(fake_lv2_peek_count == peeks);
	g_cfg.enable_shim = 1;
	CHECK(kshim_install() == KSHIM_NO_ENTRY);
	CHECK(fake_lv2_peek_count > peeks);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_second_install_adopts(void)
{
	int pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* a second install finds the redirects already pointing at a blob whose
	 * data block carries the magic: it adopts and writes nothing */
	g_spare_calls = 0;
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	CHECK(fake_lv2_poke_count == pokes);   /* zero pokes */
	CHECK(g_spare_calls == 0);             /* nothing re-allocated */
	CHECK(rd(STARTUP_SLOT) == BLOB_BASE + DS45_SHIM_OFF_STARTUP);

	kshim_remove();
	return 0;
}

static int test_remove_restores(void)
{
	struct snap before, after;
	int pokes;

	setup();
	take_snap(&before);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(STARTUP_SLOT) != before.startup);   /* really changed */
	CHECK(rd(PID0_QA) != PID0_Q && rd(PID1_QA) != PID1_Q && rd(PID2_QA) != PID2_Q);

	/* the journal holds one entry per patch - the three product-ID patches and the
	 * two redirects - so remove writes back exactly five qwords */
	pokes = fake_lv2_poke_count;
	kshim_remove();
	CHECK(fake_lv2_poke_count - pokes == 5);
	take_snap(&after);
	CHECK(snap_eq(&before, &after));             /* redirects and patches back */
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(PID0_QA) == PID0_Q);
	CHECK(rd(PID1_QA) == PID1_Q);
	CHECK(rd(PID2_QA) == PID2_Q);

	/* the blob itself is left allocated (a driver thread may be inside it) */
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == DS45_SHIM_MAGIC);

	/* remove again is a harmless no-op */
	kshim_remove();
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	return 0;
}

/* The built-in table carries exactly the one researched DEX 4.92 row. */
static int test_builtin_table(void)
{
	const ds45_shim_kernel_t *k = &ds45_shim_table[0];

	CHECK(ds45_shim_table_count == 1);
	CHECK(k->spare_syscall == 149u);
	CHECK(k->alloc_pool == 0x22ULL);

	/* F4 (note 2026-10-31-ds3-route-pid-patch-fix.md, Option A): exactly three
	 * product-ID rows, pinned verbatim. The two inline-site rows are WORD patches
	 * with their finished instruction (cmpwi cr7,r0,0x0CE6 and b 0x11B234); only
	 * the helper's entry is a BRANCH, whose target is filled in at install time.
	 * A BRANCH at the inline site is the bug that wedged the console. */
	CHECK(DS45_SHIM_PID_PATCHES == 3);
	CHECK(sizeof k->pid_patches / sizeof k->pid_patches[0] == 3);
	CHECK(k->pid_patches[0].kind == DS45_KPATCH_WORD);
	CHECK(k->pid_patches[0].addr == 0x800000000011B5E4ULL);
	CHECK(k->pid_patches[0].expect == 0xE8ADD0EDF3B2B3C1ULL);
	CHECK(k->pid_patches[0].value == 0x2F800CE6ULL);
	CHECK(k->pid_patches[1].kind == DS45_KPATCH_WORD);
	CHECK(k->pid_patches[1].addr == 0x800000000011B5ECULL);
	CHECK(k->pid_patches[1].expect == 0xF86140C9C3D7FB8DULL);
	CHECK(k->pid_patches[1].value == 0x4BFFFC48ULL);
	CHECK(k->pid_patches[2].kind == DS45_KPATCH_BRANCH);
	CHECK(k->pid_patches[2].addr == 0x800000000029E2B4ULL);
	CHECK(k->pid_patches[2].expect == 0xF1B8B3EFF22A78FCULL);
	CHECK(k->pid_patches[2].value == 0);                    /* filled at install */

	/* the kernel's BT-HID transfer routine h116ef0 (note P1/P4): its entry, and
	 * the one kernel TOC every OPD on this firmware carries (fact sheet header) */
	CHECK(ds45_shim_table[0].send_entry == 0x8000000000116EF0ULL);
	CHECK(ds45_shim_table[0].send_toc == 0x80000000003759C0ULL);
	CHECK(ds45_shim_table[0].send_toc == KERNEL_TOC);
	return 0;
}

/* A row with no transfer routine installs, but leaves the descriptor and the
 * pointer to it zero - the shim's DualSense start-up then fails safe. */
static int test_install_without_send_routine(void)
{
	uint8_t db[304];
	int i;

	setup();
	g_ent.send_entry = 0;
	g_ent.send_toc = 0;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	CHECK(sizeof(ds45_shim_data_t) == sizeof db);
	CHECK(ds45_lv2_read_bytes(BLOB_BASE + DS45_SHIM_OFF_DATA, db, (int)sizeof db) == 0);
	CHECK(be64b(db + offsetof(ds45_shim_data_t, send_routine)) == 0);
	for (i = 0; i < 3; i++)
		CHECK(be64b(db + offsetof(ds45_shim_data_t, send_opd) + 8 * i) == 0);

	kshim_remove();
	return 0;
}

/* A blob left by a build with a different data-block layout is NOT adopted: the
 * plugin would reach into it at offsets that blob does not use. Nothing is
 * written; the install is refused. */
static int test_adopt_requires_the_same_version(void)
{
	int pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* same magic, another version: the version is the high word of the qword at
	 * data+8 (its low word is `enabled`) */
	CHECK(offsetof(ds45_shim_data_t, version) == 8);
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + 8,
	      ((uint64_t)(DS45_SHIM_VERSION - 1) << 32) | 1);

	g_spare_calls = 0;
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(fake_lv2_poke_count == pokes);   /* zero pokes */
	CHECK(g_spare_calls == 0);             /* nothing allocated */
	CHECK(rd(STARTUP_SLOT) == BLOB_BASE + DS45_SHIM_OFF_STARTUP);   /* left as found */

	/* the current version is adopted (test_second_install_adopts) */
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + 8,
	      ((uint64_t)DS45_SHIM_VERSION << 32) | 1);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_remove();
	return 0;
}

/* ----- kshim_set_connection: the plugin-side cache of the per-connection id ----- */

#define CONN_A  0x0123456789ABCDEFULL
#define CONN_B  0xFEDCBA9876543210ULL
#define CONN_C  0x00000000DEADBEEFULL

static uint64_t db_field(size_t off)
{
	return rd(BLOB_BASE + DS45_SHIM_OFF_DATA + off);
}

static uint64_t slot_key(int i)
{
	return db_field(offsetof(ds45_shim_data_t, conn_key) + 8 * (size_t)i);
}

static uint64_t slot_id(int i)
{
	return db_field(offsetof(ds45_shim_data_t, conn_id) + 8 * (size_t)i);
}

/* The blob's count of start-up sends for slot i (start_tries[] is a qword per
 * slot, so the plugin can zero one without touching its neighbours). */
static uint64_t slot_tries(int i)
{
	return db_field(offsetof(ds45_shim_data_t, start_tries) + 8 * (size_t)i);
}

/* What the blob does as it sends: bump the slot's count. Used to put the live
 * block in a state the plugin has to deal with - a spent budget, or the stale
 * count a slot's previous occupant left behind. */
static void blob_sent(int i, uint64_t n)
{
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, start_tries) +
	      8 * (size_t)i, n);
}

/* conn_state[] is DS45_SHIM_CONNS bytes = one big-endian qword: byte 0 is its
 * most significant byte. */
static int slot_state(int i)
{
	return (int)((db_field(offsetof(ds45_shim_data_t, conn_state)) >>
	              (56 - 8 * i)) & 0xFF);
}

/* Nothing in the data block outside the per-connection arrays changed. */
static int rest_of_block_intact(void)
{
	uint8_t db[304];
	const uint8_t *cfg;
	int i;

	CHECK_SUB(sizeof(ds45_shim_data_t) == sizeof db ? 0 : __LINE__);
	CHECK_SUB(ds45_lv2_read_bytes(BLOB_BASE + DS45_SHIM_OFF_DATA, db,
	                              (int)sizeof db) == 0 ? 0 : __LINE__);
	/* in the tests that use this the blob never ran, so every budget is full */
	for (i = 0; i < DS45_SHIM_CONNS; i++)
		CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, start_tries) + 8 * i) ==
		          0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, magic)) ==
	          DS45_SHIM_MAGIC ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, version)) ==
	          DS45_SHIM_VERSION ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, enabled)) == 1 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_startup)) ==
	          STARTUP_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_input)) ==
	          INPUT_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_routine)) ==
	          BLOB_BASE + DS45_SHIM_OFF_DATA +
	          offsetof(ds45_shim_data_t, send_opd) ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_opd)) ==
	          SEND_ENTRY ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, send_opd) + 8) ==
	          SEND_TOC ? 0 : __LINE__);
	cfg = db + offsetof(ds45_shim_data_t, cfg);
	CHECK_SUB(cfg[offsetof(ds45_shim_cfg_t, deadzone)] == 12 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pdu)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_startups)) == 0 ? 0 : __LINE__);
	return 0;
}

static int test_set_connection_needs_an_install(void)
{
	int pokes;

	setup();
	pokes = fake_lv2_poke_count;
	CHECK(kshim_set_connection(0x2A, CONN_A) == -1);   /* nothing installed yet */
	CHECK(fake_lv2_poke_count == pokes);

	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	kshim_remove();
	pokes = fake_lv2_poke_count;
	CHECK(kshim_set_connection(0x2A, CONN_A) == -1);   /* removed again */
	CHECK(fake_lv2_poke_count == pokes);
	return 0;
}

static int test_set_connection_fills_the_slot(void)
{
	int i, pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* a new handle takes the first free slot: its handle, its id, state = 1 */
	CHECK(kshim_set_connection(0x2A, CONN_A) == 0);
	CHECK(slot_key(0) == 0x2A);
	CHECK(slot_id(0) == CONN_A);                       /* all 64 bits */
	CHECK(slot_state(0) == 1);
	for (i = 1; i < DS45_SHIM_CONNS; i++)
		CHECK(slot_key(i) == 0 && slot_id(i) == 0 && slot_state(i) == 0);

	/* another handle, the next free slot */
	CHECK(kshim_set_connection(0x2B, CONN_B) == 0);
	CHECK(slot_key(1) == 0x2B && slot_id(1) == CONN_B && slot_state(1) == 1);
	CHECK(slot_id(0) == CONN_A && slot_state(0) == 1); /* the first is untouched */
	CHECK(slot_state(2) == 0);

	/* the same handle again (the pad reconnected): updated in place, no new slot */
	CHECK(kshim_set_connection(0x2A, CONN_C) == 0);
	CHECK(slot_id(0) == CONN_C && slot_key(0) == 0x2A && slot_state(0) == 1);
	CHECK(slot_state(2) == 0);

	/* a zero id forgets the handle: its state goes to 0 so the shim stops
	 * matching it; the other connection is untouched */
	CHECK(kshim_set_connection(0x2A, 0) == 0);
	CHECK(slot_state(0) == 0);
	CHECK(slot_id(1) == CONN_B && slot_key(1) == 0x2B && slot_state(1) == 1);

	/* forgetting a handle that is not recorded writes nothing */
	pokes = fake_lv2_poke_count;
	CHECK(kshim_set_connection(0x99, 0) == 0);
	CHECK(fake_lv2_poke_count == pokes);

	/* the freed slot is reused */
	CHECK(kshim_set_connection(0x3C, CONN_A) == 0);
	CHECK(slot_key(0) == 0x3C && slot_id(0) == CONN_A && slot_state(0) == 1);

	/* nothing outside the per-connection arrays moved, and nothing wild */
	CHECK(rest_of_block_intact() == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	kshim_remove();
	return 0;
}

static int test_set_connection_table_full(void)
{
	int i, pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	for (i = 0; i < DS45_SHIM_CONNS; i++)
		CHECK(kshim_set_connection((uint8_t)(0x10 + i), CONN_A + (uint64_t)i) == 0);

	/* a ninth handle has no slot: refused without a write ... */
	pokes = fake_lv2_poke_count;
	CHECK(kshim_set_connection(0x77, CONN_B) == -1);
	CHECK(fake_lv2_poke_count == pokes);
	/* ... while an existing one still updates in place */
	CHECK(kshim_set_connection(0x13, CONN_B) == 0);
	CHECK(slot_id(3) == CONN_B && slot_key(3) == 0x13);
	for (i = 0; i < DS45_SHIM_CONNS; i++)
		CHECK(slot_state(i) == 1);

	CHECK(rest_of_block_intact() == 0);
	kshim_remove();
	return 0;
}

/* A slot becomes visible to the shim only when its state byte is set, and that
 * is written last - after its id, its handle and the re-arm of its send budget:
 * if any earlier write does not stick, the shim never sees a half-written
 * connection (nor one that would start with a stale, possibly spent, budget). */
static int test_set_connection_publishes_last(void)
{
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* slot 0 still carries a count its previous occupant left behind, so a
	 * dropped re-arm is visible (a dropped write of 0 over 0 would read back) */
	blob_sent(0, 6);

	/* the id lands, the handle write is dropped */
	fake_lv2_poke_fail_after = fake_lv2_poke_count + 1;
	CHECK(kshim_set_connection(0x2A, CONN_A) == -1);
	fake_lv2_poke_fail_after = -1;
	CHECK(slot_state(0) == 0);

	/* the id and the handle land, the re-arm is dropped */
	fake_lv2_poke_fail_after = fake_lv2_poke_count + 2;
	CHECK(kshim_set_connection(0x2A, CONN_A) == -1);
	fake_lv2_poke_fail_after = -1;
	CHECK(slot_state(0) == 0);
	CHECK(slot_tries(0) == 6);                 /* really not re-armed */

	/* the id, the handle and the re-arm land, the state write is dropped: the
	 * slot is armed but still not visible */
	fake_lv2_poke_fail_after = fake_lv2_poke_count + 3;
	CHECK(kshim_set_connection(0x2A, CONN_A) == -1);
	fake_lv2_poke_fail_after = -1;
	CHECK(slot_state(0) == 0);
	CHECK(slot_tries(0) == 0);

	/* and it works once the writes stick */
	CHECK(kshim_set_connection(0x2A, CONN_A) == 0);
	CHECK(slot_state(0) == 1 && slot_id(0) == CONN_A && slot_key(0) == 0x2A);
	CHECK(slot_tries(0) == 0);

	/* forgetting clears the state FIRST: a dropped write leaves the slot
	 * still matching rather than half-torn-down... */
	fake_lv2_poke_fail_after = fake_lv2_poke_count;    /* every poke dropped */
	CHECK(kshim_set_connection(0x2A, 0) == -1);
	fake_lv2_poke_fail_after = -1;
	CHECK(slot_state(0) == 1 && slot_id(0) == CONN_A);

	/* ... and a clean forget stops the shim matching it */
	CHECK(kshim_set_connection(0x2A, 0) == 0);
	CHECK(slot_state(0) == 0);

	kshim_remove();
	return 0;
}

/* The shim sends its start-up GET at most SHIM_START_TRIES_MAX times per slot
 * (the blob counts them in start_tries[slot]; shim.c's own tests cover the cap).
 * kshim_set_connection is what gives a connection a fresh budget: it zeroes the
 * slot's count - one qword, no neighbour touched - when it publishes a new
 * connection, and when a handle comes back with a different id. The same
 * connection reported again changes nothing, so a caller that reports it on
 * every rescan cannot keep refilling the budget. */
static int test_set_connection_rearms_the_send_budget(void)
{
	int pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* a first connection starts with a full budget; the shim then spends it all */
	CHECK(kshim_set_connection(0x2A, CONN_A) == 0);
	CHECK(slot_tries(0) == 0);
	blob_sent(0, 8);

	/* the same connection reported again: no write at all, budget not refilled */
	pokes = fake_lv2_poke_count;
	CHECK(kshim_set_connection(0x2A, CONN_A) == 0);
	CHECK(fake_lv2_poke_count == pokes);
	CHECK(slot_tries(0) == 8);

	/* the handle comes back with a new id (the pad reconnected): that is a new
	 * connection, so slot 0's budget is refilled - and only slot 0's. Slots 1
	 * and 2 hold counts the blob left on them; slot 1 is not in use. */
	blob_sent(1, 5);
	blob_sent(2, 8);
	CHECK(kshim_set_connection(0x2A, CONN_B) == 0);
	CHECK(slot_id(0) == CONN_B && slot_state(0) == 1);
	CHECK(slot_tries(0) == 0);
	CHECK(slot_tries(1) == 5 && slot_tries(2) == 8);

	/* a new connection taking a free slot whose previous occupant left a count
	 * behind (slot 1: 5) starts with a full budget */
	CHECK(kshim_set_connection(0x2B, CONN_C) == 0);
	CHECK(slot_key(1) == 0x2B && slot_state(1) == 1);
	CHECK(slot_tries(1) == 0);
	CHECK(slot_tries(0) == 0 && slot_tries(2) == 8);   /* neighbours untouched */

	/* forgetting a connection leaves its count alone; publishing the handle
	 * again is a new connection and refills it */
	blob_sent(1, 4);
	CHECK(kshim_set_connection(0x2B, 0) == 0);
	CHECK(slot_state(1) == 0 && slot_tries(1) == 4);
	CHECK(kshim_set_connection(0x2B, CONN_C) == 0);
	CHECK(slot_state(1) == 1 && slot_tries(1) == 0);

	CHECK(fake_lv2_out_of_bounds == 0);
	kshim_remove();
	return 0;
}

int run_tests(void)
{
	int line;

	line = test_blob_tail_is_the_data_block();
	if (line)
		return line;
	line = test_no_entry_writes_nothing();
	if (line)
		return line;
	line = test_install_happy_path();
	if (line)
		return line;
	line = test_install_fails_each_step();
	if (line)
		return line;
	line = test_install_refuses_each_unexpected_pid_site();
	if (line)
		return line;
	line = test_word_rows_do_not_follow_the_load_base();
	if (line)
		return line;
	line = test_install_rolls_back_a_partial_patch();
	if (line)
		return line;
	line = test_install_is_gated_by_enable_shim();
	if (line)
		return line;
	line = test_second_install_adopts();
	if (line)
		return line;
	line = test_remove_restores();
	if (line)
		return line;
	line = test_builtin_table();
	if (line)
		return line;
	line = test_install_without_send_routine();
	if (line)
		return line;
	line = test_adopt_requires_the_same_version();
	if (line)
		return line;
	line = test_set_connection_needs_an_install();
	if (line)
		return line;
	line = test_set_connection_fills_the_slot();
	if (line)
		return line;
	line = test_set_connection_table_full();
	if (line)
		return line;
	line = test_set_connection_publishes_last();
	if (line)
		return line;
	line = test_set_connection_rearms_the_send_budget();
	if (line)
		return line;

	return 0;
}
