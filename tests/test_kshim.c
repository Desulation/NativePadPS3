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
#define GET_REPORT   0x8000000000403000ULL
#define PID0_SITE    (B + 0x7000)
#define PID0_Q       0x2C0303D541820010ULL
#define PID1_SITE    (B + 0x8000)
#define PID1_Q       0x2C03054C2C040268ULL

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
	put_q(PID0_SITE, PID0_Q);
	put_q(PID1_SITE, PID1_Q);

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
	g_ent.get_report_desc = GET_REPORT;
	g_ent.pid_patches[0].kind = DS45_KPATCH_BRANCH;
	g_ent.pid_patches[0].addr = PID0_SITE;
	g_ent.pid_patches[0].expect = qhash(PID0_Q);
	g_ent.pid_patches[0].value = 0;
	g_ent.pid_patches[1].kind = DS45_KPATCH_BRANCH;
	g_ent.pid_patches[1].addr = PID1_SITE;
	g_ent.pid_patches[1].expect = qhash(PID1_Q);
	g_ent.pid_patches[1].value = 0;
}

/* ----- tests ------------------------------------------------------------- */

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
	uint8_t db[144];                 /* sizeof(ds45_shim_data_t), 8-aligned */
	const uint8_t *cfg;
	uint32_t off;

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
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, get_report)) ==
	          GET_REPORT ? 0 : __LINE__);

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
	/* per-connection and counter fields start cleared */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, conn_key)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_translated)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_startups)) == 0 ? 0 : __LINE__);

	/* flush OPD placed just past the blob: { entry, kernel TOC } */
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN) == FLUSH_ENTRY ? 0 : __LINE__);
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN + 8) == KERNEL_TOC ? 0 : __LINE__);
	return 0;
}

static int test_install_happy_path(void)
{
	uint32_t bw0 = 0, bw1 = 0;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	/* redirects now point at the blob's own descriptors */
	CHECK(rd(STARTUP_SLOT) == BLOB_BASE + DS45_SHIM_OFF_STARTUP);
	CHECK(rd(INPUT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_INPUT);

	/* product-ID sites now branch to shim_pid_ok (high half of each qword) */
	CHECK(kpatch_branch_word(PID0_SITE, BLOB_BASE + DS45_SHIM_OFF_PID_OK, &bw0) == 0);
	CHECK(kpatch_branch_word(PID1_SITE, BLOB_BASE + DS45_SHIM_OFF_PID_OK, &bw1) == 0);
	CHECK((uint32_t)(rd(PID0_SITE) >> 32) == bw0);
	CHECK((uint32_t)(PID0_Q & 0xFFFFFFFFULL) == (uint32_t)rd(PID0_SITE)); /* low half kept */
	CHECK((uint32_t)(rd(PID1_SITE) >> 32) == bw1);

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
struct snap { uint64_t startup, input, spare, pid0, pid1; };

static void take_snap(struct snap *s)
{
	s->startup = rd(STARTUP_SLOT);
	s->input   = rd(INPUT_SLOT);
	s->spare   = rd(SPARE_SLOT);
	s->pid0    = rd(PID0_SITE);
	s->pid1    = rd(PID1_SITE);
}

static int snap_eq(const struct snap *a, const struct snap *b)
{
	return a->startup == b->startup && a->input == b->input &&
	       a->spare == b->spare && a->pid0 == b->pid0 && a->pid1 == b->pid1;
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
	g_fault_at = 60;                       /* well into the blob copy */
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
	CHECK(rd(PID0_SITE) == before.pid0);
	CHECK(rd(PID1_SITE) == before.pid1);
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	/* restore the test's artefact, then the whole set is byte-identical */
	put_q(INPUT_SLOT, INPUT_ORIG);
	take_snap(&after);
	CHECK(snap_eq(&before, &after));

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

	setup();
	take_snap(&before);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(STARTUP_SLOT) != before.startup);   /* really changed */

	kshim_remove();
	take_snap(&after);
	CHECK(snap_eq(&before, &after));             /* redirects and patches back */
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(PID0_SITE) == PID0_Q);
	CHECK(rd(PID1_SITE) == PID1_Q);

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
	CHECK(ds45_shim_table_count == 1);
	CHECK(ds45_shim_table[0].spare_syscall == 149u);
	CHECK(ds45_shim_table[0].alloc_pool == 0x22ULL);
	CHECK(ds45_shim_table[0].pid_patches[0].kind == DS45_KPATCH_BRANCH);
	CHECK(ds45_shim_table[0].pid_patches[0].value == 0);   /* filled at install */
	return 0;
}

int run_tests(void)
{
	int line;

	line = test_no_entry_writes_nothing();
	if (line)
		return line;
	line = test_install_happy_path();
	if (line)
		return line;
	line = test_install_fails_each_step();
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

	return 0;
}
