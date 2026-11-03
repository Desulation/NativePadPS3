/* SOURCES: lv2.c kpatch.c kshim.c kshim_table.c */
/* CFLAGS: -DDS45_DEBUG */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Synthetic image only: made-up fingerprint bytes, code qwords and in-window
 * addresses, no kernel bytes. The one real input is src/shim_blob.h (the freshly
 * packed blob the VSH build generates), so the relocation and data-block checks
 * run against the exact bytes the installer copies. */

#define B DS45_LV2_BASE

/* Table-entry addresses (all in-window, chosen clear of one another and of the
 * blob region). The three driver slots sit in one table as on DEX 4.92: the
 * enable driver's event slot, the start-up driver's send slot (which v3 never
 * writes) and the start-up driver's event slot. */
#define FP_ADDR      (B + 0x1000)
#define SYS_TABLE    (B + 0x2000)
#define SPARE_NUM    149u
#define SPARE_SLOT   (SYS_TABLE + (uint64_t)SPARE_NUM * 8)   /* B + 0x24A8 */
#define SPARE_STUB   0x80000000003621B0ULL                  /* ni stub OPD */
#define ALLOC_DESC   (B + 0x3000)
#define ALLOC_ENTRY  0x80000000000681F4ULL
#define KERNEL_TOC   0x80000000003759C0ULL   /* at ALLOC_DESC + 8: the kernel TOC */
#define FLUSH_ENTRY  0x80000000000029B8ULL
#define ENABLE_SLOT  (B + 0x6000)
#define ENABLE_ORIG  0x800000000033A310ULL
#define STARTUP_SLOT (B + 0x6038)
#define STARTUP_ORIG 0x800000000033A208ULL
#define EVENT_SLOT   (B + 0x6048)
#define EVENT_ORIG   0x800000000033A2B0ULL
#define INPUT_SLOT   (B + 0x6100)
#define INPUT_ORIG   0x800000000033A010ULL
/* The two handlers' first code qwords. The event handler is 8-aligned; the
 * enable handler, like the real 0xCB234, is not, so its hash covers the
 * qword that holds it. Made-up patterns. */
#define EVENT_CODE    (B + 0x6800)
#define EVENT_CODE_Q  0x1111222233334444ULL
#define ENABLE_CODE   (B + 0x6904)
#define ENABLE_CODE_QA (B + 0x6900)
#define ENABLE_CODE_Q 0x5555666677778888ULL
/* The values the data block carries for the shim; never dereferenced here. */
#define PAD_VTABLE   0x800000000038CDC8ULL
#define PAD_MGR      0x80000000004A1CB8ULL

/* The product-ID WORD rows, shaped like the DEX 4.92 row: the patched
 * instruction is the LOW half of its qword (addr & 4 set) and the high half is a
 * neighbouring instruction that has to survive. The original qwords are made-up
 * patterns, not kernel bytes. */
#define HELPER_QA    (B + 0x8000)          /* helper: the qword's address ... */
#define HELPER_SITE  (HELPER_QA + 4)       /* ... and the instruction in it */
#define HELPER_Q     0x5E5E5E5E6F6F6F6FULL
#define HELPER_WORD  0x68800CE6u
#define PID0_QA      (B + 0x7000)          /* inline compare */
#define PID0_SITE    (PID0_QA + 4)
#define PID0_Q       0x1A1A1A1A2B2B2B2BULL
#define PID0_WORD    0x2F800CE6u
#define PID1_QA      (B + 0x7008)          /* inline branch */
#define PID1_SITE    (PID1_QA + 4)
#define PID1_Q       0x3C3C3C3C4D4D4D4DULL
#define PID1_WORD    0x4BFFFC48u
#define HI32         0xFFFFFFFF00000000ULL

/* The fixed executable base the blob is placed at (design Amendment 2): an
 * in-window address clear of every table-entry address above, standing in for
 * the real DEX 4.92 free zero tail. EXEC_BOUND leaves a 0x4000 region, like the
 * real 0x57C000..0x580000. */
#define BLOB_BASE    (B + 0x600000)
#define EXEC_BOUND   (BLOB_BASE + 0x4000)

#define TOCTOU_VAL   0x8000000000BAD008ULL   /* a "concurrent" slot change */

#define CHECK_SUB(e) do { int l_ = (e); if (l_) return l_; } while (0)

/* A synthetic info template: a pattern with the identity bytes zero. */
static uint8_t g_tmpl[DS45_SHIM_INFO_LEN];

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

/* The debug trace (DS45_DEBUG): overrides the weak fake to catch the adopted
 * product-ID mask. */
static int      g_tr_applied_n;
static uint32_t g_tr_applied;

void ds45_boot_mark_hex(const char *stage, uint32_t value)
{
	if (strcmp(stage, "kshim_adopt_applied") == 0) {
		g_tr_applied_n++;
		g_tr_applied = value;
	}
}

/* ----- the spare-syscall stand-in ---------------------------------------- */

static int      g_spare_calls;
static uint64_t g_spare_a1[4], g_spare_a2[4];

/* The install borrows the spare slot once, for the 0x29B8 cache flush; its
 * result is ignored. The blob is placed at a fixed executable base, so there is
 * no allocator call any more. */
static uint64_t spare_handler(uint64_t a1, uint64_t a2)
{
	if (g_spare_calls < 4) {
		g_spare_a1[g_spare_calls] = a1;
		g_spare_a2[g_spare_calls] = a2;
	}
	g_spare_calls++;
	return 0;
}

/* The probe borrows the spare slot twice: the 0x29B8 flush, then one call into
 * the leaf stub at exec_base. The real dispatcher double-derefs the slot (slot
 * -> OPD -> entry); the fake does not model that, so do it here: read the OPD
 * the probe pointed the slot at, record the entry and args per call, and when
 * the entry lands inside the exec region emulate running `li r3,imm; blr` by
 * returning the stub's li immediate. The flush call (entry 0x29B8, outside the
 * region) returns 0. */
static int      g_probe_calls;
static uint64_t g_probe_entry[4], g_probe_a1[4], g_probe_a2[4];
static uint64_t g_probe_stub;    /* the stub qword the exec call ran */
static uint64_t probe_spare_handler(uint64_t a1, uint64_t a2)
{
	uint64_t opd = rd(SPARE_SLOT);
	uint64_t entry = rd(opd);

	if (g_probe_calls < 4) {
		g_probe_entry[g_probe_calls] = entry;
		g_probe_a1[g_probe_calls] = a1;
		g_probe_a2[g_probe_calls] = a2;
	}
	g_probe_calls++;
	if (entry >= BLOB_BASE && entry < EXEC_BOUND) {
		g_probe_stub = rd(entry);
		return (uint64_t)((uint32_t)(g_probe_stub >> 32) & 0xFFFF);
	}
	return 0;
}

/* The transient-fault helper from test_kpatch: drop pokes from fail_after on,
 * then lift the fault as soon as the first dropped poke has been read back, so
 * the code under test can still land its own rollback / clean-up writes. */
static int g_fault_at;
static void clear_fault_after_failed_poke(uint64_t addr)
{
	(void)addr;
	if (fake_lv2_poke_count > g_fault_at)
		fake_lv2_poke_fail_after = -1;
}

/* ----- the LV2 write log -------------------------------------------------- */

/* ds45_lv2_write reads every poke back at the same address, so the first peek
 * after the poke count moved names the address just written. */
#define WLOG_MAX 4096
static uint64_t g_wlog[WLOG_MAX];
static int      g_wlog_n, g_wlog_pokes;

static void wlog_on_peek(uint64_t addr)
{
	if (fake_lv2_poke_count != g_wlog_pokes) {
		g_wlog_pokes = fake_lv2_poke_count;
		if (g_wlog_n < WLOG_MAX)
			g_wlog[g_wlog_n++] = addr;
	}
}

static void wlog_start(void)
{
	g_wlog_n = 0;
	g_wlog_pokes = fake_lv2_poke_count;
	fake_lv2_on_peek = wlog_on_peek;
}

static int wlog_has(uint64_t addr)
{
	int i;
	for (i = 0; i < g_wlog_n; i++)
		if (g_wlog[i] == addr)
			return 1;
	return 0;
}

/* ----- image + table setup ----------------------------------------------- */

static ds45_shim_kernel_t g_ent;

static void setup(void)
{
	uint8_t fp[DS45_FP_LEN];
	int i;

	fake_reset_all();
	g_spare_calls = 0;
	g_probe_calls = 0;
	g_probe_stub = 0;
	memset(g_spare_a1, 0, sizeof(g_spare_a1));
	memset(g_spare_a2, 0, sizeof(g_spare_a2));
	memset(g_probe_entry, 0, sizeof(g_probe_entry));
	memset(g_probe_a1, 0, sizeof(g_probe_a1));
	memset(g_probe_a2, 0, sizeof(g_probe_a2));
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
	g_cfg.shim_inline_pid = 0;
	g_cfg.shim_input_hook = 1;   /* the input redirect installs as today */
	g_cfg.shim_event_hook = 1;   /* event/enable redirects on by default */
	g_cfg.shim_enable_hook = 1;

	/* fingerprint */
	for (i = 0; i < DS45_FP_LEN; i++)
		fp[i] = (uint8_t)(0x40 + i * 7);
	fake_lv2_set(FP_ADDR, fp, DS45_FP_LEN);

	/* slots, code qwords and the allocator OPD */
	put_q(SPARE_SLOT, SPARE_STUB);
	put_q(ALLOC_DESC, ALLOC_ENTRY);
	put_q(ALLOC_DESC + 8, KERNEL_TOC);
	put_q(EVENT_SLOT, EVENT_ORIG);
	put_q(ENABLE_SLOT, ENABLE_ORIG);
	put_q(STARTUP_SLOT, STARTUP_ORIG);
	put_q(INPUT_SLOT, INPUT_ORIG);
	put_q(EVENT_CODE, EVENT_CODE_Q);
	put_q(ENABLE_CODE_QA, ENABLE_CODE_Q);
	put_q(HELPER_QA, HELPER_Q);
	put_q(PID0_QA, PID0_Q);
	put_q(PID1_QA, PID1_Q);

	for (i = 0; i < DS45_SHIM_INFO_LEN; i++)
		g_tmpl[i] = (uint8_t)(0x21 + i * 3);
	for (i = 8; i < 16; i++)
		g_tmpl[i] = 0;

	/* synthetic table row */
	memset(&g_ent, 0, sizeof(g_ent));
	g_ent.name = "SYN";
	g_ent.fp_addr = FP_ADDR;
	g_ent.fp_hash = ds45_fnv1a64(fp, DS45_FP_LEN);
	g_ent.syscall_table = SYS_TABLE;
	g_ent.spare_syscall = SPARE_NUM;
	g_ent.spare_slot_expect = SPARE_STUB;
	g_ent.alloc_desc = ALLOC_DESC;
	g_ent.exec_base = BLOB_BASE;
	g_ent.exec_bound = EXEC_BOUND;
	g_ent.flush_desc = FLUSH_ENTRY;
	g_ent.event_slot = EVENT_SLOT;
	g_ent.event_expect = EVENT_ORIG;
	g_ent.event_code_addr = EVENT_CODE;
	g_ent.event_code_hash = qhash(EVENT_CODE_Q);
	g_ent.enable_slot = ENABLE_SLOT;
	g_ent.enable_expect = ENABLE_ORIG;
	g_ent.enable_code_addr = ENABLE_CODE;
	g_ent.enable_code_hash = qhash(ENABLE_CODE_Q);
	g_ent.input_slot = INPUT_SLOT;
	g_ent.input_expect = INPUT_ORIG;
	g_ent.pad_vtable = PAD_VTABLE;
	g_ent.pad_mgr = PAD_MGR;
	g_ent.info_tmpl = g_tmpl;
	g_ent.pid_patches[0].kind = DS45_KPATCH_WORD;
	g_ent.pid_patches[0].addr = HELPER_SITE;
	g_ent.pid_patches[0].expect = qhash(HELPER_Q);
	g_ent.pid_patches[0].value = HELPER_WORD;
	g_ent.inline_patches[0].kind = DS45_KPATCH_WORD;
	g_ent.inline_patches[0].addr = PID0_SITE;
	g_ent.inline_patches[0].expect = qhash(PID0_Q);
	g_ent.inline_patches[0].value = PID0_WORD;
	g_ent.inline_patches[1].kind = DS45_KPATCH_WORD;
	g_ent.inline_patches[1].addr = PID1_SITE;
	g_ent.inline_patches[1].expect = qhash(PID1_Q);
	g_ent.inline_patches[1].value = PID1_WORD;
}

/* Snapshot of everything the installer is responsible for leaving untouched on
 * failure (the fixed blob region is scratch and excluded). */
struct snap { uint64_t event, enable, startup, input, spare, helper, pid0, pid1,
                       ecode, ncode; };

static void take_snap(struct snap *s)
{
	s->event   = rd(EVENT_SLOT);
	s->enable  = rd(ENABLE_SLOT);
	s->startup = rd(STARTUP_SLOT);
	s->input   = rd(INPUT_SLOT);
	s->spare   = rd(SPARE_SLOT);
	s->helper  = rd(HELPER_QA);
	s->pid0    = rd(PID0_QA);
	s->pid1    = rd(PID1_QA);
	s->ecode   = rd(EVENT_CODE);
	s->ncode   = rd(ENABLE_CODE_QA);
}

static int snap_eq(const struct snap *a, const struct snap *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* A refusal before anything is written, borrowed or allocated (defined below,
 * forward-declared so the install-placement tests can use it). */
static int refused_untouched(const struct snap *before, int pokes);

/* The plugin unloads in two steps: unhook (revert the redirects and words,
 * confirm they stuck, arm the scrub) then, after a short drain on the console,
 * scrub (zero the fixed blob region). The host has no concurrent blob thread, so
 * the tests call them back to back; most tests want the full effect, so this
 * stands in for the old one-shot remove. Split-specific tests call the two
 * directly. */
static void kshim_remove(void)
{
	kshim_unhook();
	kshim_scrub();
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
	uint8_t db[sizeof(ds45_shim_data_t)];
	const uint8_t *cfg, *t;
	uint32_t off;
	int i;

	for (off = 0; off < DS45_SHIM_OFF_DATA; off += 8) {
		uint64_t want = blob_q(off);
		if (is_reloc(off))
			want += BLOB_BASE;
		CHECK_SUB(rd(BLOB_BASE + off) == want ? 0 : __LINE__);
	}

	CHECK_SUB(ds45_lv2_read_bytes(BLOB_BASE + DS45_SHIM_OFF_DATA, db,
	                              (int)sizeof db) == 0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, magic)) ==
	          DS45_SHIM_MAGIC ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, version)) == 5 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, enabled)) == 1 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_event)) ==
	          EVENT_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_enable)) ==
	          ENABLE_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_input)) ==
	          INPUT_ORIG ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pad_vtable)) ==
	          PAD_VTABLE ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pad_mgr)) ==
	          PAD_MGR ? 0 : __LINE__);

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

	/* counters start cleared */
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_completed)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_enabled)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_translated)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stat_passed)) == 0 ? 0 : __LINE__);

	/* the row's template, byte for byte, then zero to the end of the array */
	t = db + offsetof(ds45_shim_data_t, info_tmpl);
	CHECK_SUB(memcmp(t, g_tmpl, DS45_SHIM_INFO_LEN) == 0 ? 0 : __LINE__);
	for (i = DS45_SHIM_INFO_LEN; i < 0x58; i++)
		CHECK_SUB(t[i] == 0 ? 0 : __LINE__);

	/* the helper site's original qword, and only the helper marked applied
	 * (setup leaves SHIM_INLINE_PID off) */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pid_orig)) == HELPER_Q ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pid_orig) + 8) == 0 ? 0 : __LINE__);
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pid_orig) + 16) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, pid_applied)) == 1 ? 0 : __LINE__);

	/* flush OPD placed just past the blob: { entry, kernel TOC } */
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN) == FLUSH_ENTRY ? 0 : __LINE__);
	CHECK_SUB(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN + 8) == KERNEL_TOC ? 0 : __LINE__);
	return 0;
}

static int test_install_writes_v3(void)
{
	setup();
	wlog_start();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	fake_lv2_on_peek = NULL;

	/* the three redirects point at the blob's own descriptors */
	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);
	CHECK(rd(ENABLE_SLOT) == BLOB_BASE + DS45_SHIM_OFF_ENABLE);
	CHECK(rd(INPUT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_INPUT);

	/* the helper word goes in verbatim (low half), the high half is kept */
	CHECK(rd(HELPER_QA) == ((HELPER_Q & HI32) | HELPER_WORD));

	/* the start-up send slot was never written, nor the inline sites (gate off) */
	CHECK(g_wlog_n > 0);
	CHECK(wlog_has(EVENT_SLOT));
	CHECK(!wlog_has(STARTUP_SLOT));
	CHECK(rd(STARTUP_SLOT) == STARTUP_ORIG);
	CHECK(!wlog_has(PID0_QA) && !wlog_has(PID1_QA));
	CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q);

	/* the spare slot is back to the ni stub, the code is untouched */
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	CHECK(rd(EVENT_CODE) == EVENT_CODE_Q && rd(ENABLE_CODE_QA) == ENABLE_CODE_Q);

	/* the blob, data block and flush OPD are correct */
	CHECK(check_blob_image() == 0);

	/* only the flush went through the spare slot: the allocator is gone, the
	 * blob is placed at the fixed exec_base */
	CHECK(g_spare_calls == 1);
	CHECK(g_spare_a1[0] == BLOB_BASE);                 /* flush addr = exec_base */
	CHECK(g_spare_a2[0] == DS45_SHIM_BLOB_LEN);        /* flush len */

	CHECK(fake_lv2_out_of_bounds == 0);
	kshim_remove();
	return 0;
}

/* The blob is placed at the row's fixed exec_base, with no allocator call: the
 * three redirects and the data block land there, and the spare-call log records
 * only the flush. */
static int test_install_uses_exec_base(void)
{
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);
	CHECK(rd(ENABLE_SLOT) == BLOB_BASE + DS45_SHIM_OFF_ENABLE);
	CHECK(rd(INPUT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_INPUT);
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == DS45_SHIM_MAGIC);
	CHECK((uint32_t)(rd(BLOB_BASE + DS45_SHIM_OFF_DATA +
	                    offsetof(ds45_shim_data_t, version)) >> 32) ==
	      DS45_SHIM_VERSION);

	/* the allocator syscall was never called: the only spare call is the flush */
	CHECK(g_spare_calls == 1);
	CHECK(g_spare_a1[0] == BLOB_BASE);
	CHECK(g_spare_a2[0] == DS45_SHIM_BLOB_LEN);
	CHECK(fake_lv2_out_of_bounds == 0);
	kshim_remove();
	return 0;
}

/* Before any write the installer verifies the whole region
 * [exec_base, exec_base + blob + flush OPD) reads as zero; a single non-zero
 * qword anywhere in it refuses the install with the image byte-identical and
 * nothing borrowed. A non-zero qword just past the region does not block it. */
static int test_install_refuses_nonzero_region(void)
{
	struct snap before;

	setup();                                 /* non-zero at the very start */
	put_q(BLOB_BASE, 0x0000000100000000ULL);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	setup();                                 /* non-zero in the flush OPD, at the end */
	put_q(BLOB_BASE + DS45_SHIM_BLOB_LEN + 8, 0x0000000000000001ULL);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	setup();                                 /* just past the checked region: harmless */
	put_q(BLOB_BASE + DS45_SHIM_BLOB_LEN + 16, 0x0000000000000001ULL);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN + 16) == 1);   /* left untouched */
	kshim_remove();
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* exec_base + blob + flush OPD must end at or before exec_bound, exec_base must
 * be 8-aligned and inside the window; any of those failing refuses the install
 * before anything is read, written or borrowed. */
static int test_install_refuses_over_bound(void)
{
	struct snap before;

	setup();                                 /* no room for the flush OPD */
	g_ent.exec_bound = BLOB_BASE + DS45_SHIM_BLOB_LEN;
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	setup();                                 /* exactly enough room installs */
	g_ent.exec_bound = BLOB_BASE + DS45_SHIM_BLOB_LEN + 16;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	kshim_remove();

	setup();                                 /* a misaligned base is refused */
	g_ent.exec_base = BLOB_BASE + 4;
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* Unload zeroes the fixed blob region (it cannot be freed), restoring the
 * pristine zero fill so a later install's zero-check passes; the redirects and
 * the word edits are restored as before. */
static int test_remove_zeroes_blob(void)
{
	uint64_t off, size;
	int g;

	for (g = 0; g < 2; g++) {
		setup();
		g_cfg.shim_inline_pid = g;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == DS45_SHIM_MAGIC);

		kshim_remove();

		/* the whole region [exec_base, exec_base + blob + flush OPD) reads zero */
		size = (uint64_t)DS45_SHIM_BLOB_LEN + 16;   /* flush_desc set in setup */
		for (off = 0; off < size; off += 8)
			CHECK(rd(BLOB_BASE + off) == 0);

		/* the redirects and the helper word are back */
		CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
		CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
		CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
		CHECK(rd(HELPER_QA) == HELPER_Q);
		CHECK(fake_lv2_out_of_bounds == 0);
	}
	return 0;
}

/* The executability pre-flight pokes a two-word leaf stub (li r3,MAGIC; blr) at
 * exec_base, flushes it, calls it once through the borrowed spare slot and
 * reports what it returned; the fake spare-call returns the stub's li immediate,
 * so the probe reports MAGIC and leaves exec_base (and the two scratch OPDs)
 * zeroed with the spare slot restored. A non-zero region is refused before any
 * call. */
static int test_exec_probe_stub(void)
{
	uint64_t out = 0xDEAD;

	setup();
	fake_spare_handler = probe_spare_handler;
	CHECK(kshim_exec_probe(&g_ent, &out) == 0);
	CHECK(out == 0x5AED);
	CHECK(g_probe_calls == 2);                /* flush first, then the one exec call */

	/* call 0 is the 0x29B8 flush of the 8 stub bytes at exec_base */
	CHECK(g_probe_entry[0] == FLUSH_ENTRY);
	CHECK(g_probe_a1[0] == BLOB_BASE && g_probe_a2[0] == 8);
	/* call 1 runs the stub: its OPD entry is exec_base, holding the exact
	 * big-endian `li r3,0x5AED; blr` qword */
	CHECK(g_probe_entry[1] == BLOB_BASE);
	CHECK(g_probe_stub == 0x38605AED4E800020ULL);

	/* exec_base is left pristine: the stub and both scratch OPDs are zeroed */
	CHECK(rd(BLOB_BASE) == 0);
	CHECK(rd(BLOB_BASE + 8) == 0 && rd(BLOB_BASE + 16) == 0);
	CHECK(rd(BLOB_BASE + 24) == 0 && rd(BLOB_BASE + 32) == 0);
	/* the borrowed spare slot is back to the ni stub */
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* a non-zero region is refused before the stub is poked or any call made */
	setup();
	fake_spare_handler = probe_spare_handler;
	put_q(BLOB_BASE, 0x1234ULL);
	out = 0xDEAD;
	CHECK(kshim_exec_probe(&g_ent, &out) != 0);
	CHECK(g_probe_calls == 0);                /* no call was made */
	CHECK(out == 0);                          /* cleared */
	CHECK(rd(BLOB_BASE) == 0x1234ULL);        /* untouched */

	/* the spare slot must read as the ni stub before the probe borrows it */
	setup();
	fake_spare_handler = probe_spare_handler;
	put_q(SPARE_SLOT, SPARE_STUB + 8);        /* not the expected value */
	CHECK(kshim_exec_probe(&g_ent, &out) != 0);
	CHECK(g_probe_calls == 0);
	CHECK(rd(BLOB_BASE) == 0);                /* the stub was never poked */

	/* the 40-byte scratch span must fit under exec_bound */
	setup();
	fake_spare_handler = probe_spare_handler;
	g_ent.exec_bound = BLOB_BASE + 32;        /* 40 > 32 */
	CHECK(kshim_exec_probe(&g_ent, &out) != 0);
	CHECK(g_probe_calls == 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A probe poke failing at any forward step still leaves the spare slot restored
 * and the 40-byte scratch span zeroed (the single clean-up label runs on every
 * non-zero return). Uses the transient-fault helper so the clean-up writes land. */
static int probe_fault_at(int poke_index)
{
	uint64_t out = 0xDEAD;
	int i;

	setup();
	fake_spare_handler = probe_spare_handler;
	g_fault_at = poke_index;
	fake_lv2_poke_fail_after = poke_index;
	fake_lv2_on_peek = clear_fault_after_failed_poke;
	CHECK_SUB(kshim_exec_probe(&g_ent, &out) != 0 ? 0 : __LINE__);
	fake_lv2_on_peek = NULL;
	fake_lv2_poke_fail_after = -1;
	CHECK_SUB(rd(SPARE_SLOT) == SPARE_STUB ? 0 : __LINE__);
	for (i = 0; i < 40; i += 8)
		CHECK_SUB(rd(BLOB_BASE + i) == 0 ? 0 : __LINE__);
	CHECK_SUB(fake_lv2_out_of_bounds == 0 ? 0 : __LINE__);
	return 0;
}

static int test_probe_fault_each_step(void)
{
	int i;

	/* eight forward pokes: stub, flush OPD x2, borrow, restore, exec OPD x2,
	 * point. The four flush-stage pokes are indices 1..4. */
	for (i = 0; i < 8; i++)
		CHECK(probe_fault_at(i) == 0);
	return 0;
}

/* Unload is a two-step split: unhook reverts the redirects and words and only
 * then arms the scrub. If a revert does not stick (kpatch_revert swallows the
 * failed write), a redirect may still point into the blob, so unhook must NOT
 * arm the scrub - zeroing live blob code would send the kernel's next call to 0. */
static int test_remove_keeps_hooks_on_failed_revert(void)
{
	int pokes;

	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == DS45_SHIM_MAGIC);

	/* drop every revert poke, so the hooks stay live */
	pokes = fake_lv2_poke_count;
	fake_lv2_poke_fail_after = pokes;
	kshim_unhook();
	fake_lv2_poke_fail_after = -1;

	/* scrub must be a no-op: the blob is left intact */
	kshim_scrub();
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == DS45_SHIM_MAGIC);
	CHECK(rd(BLOB_BASE) != 0);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* The scrub target comes from the adopt path, not a prior install: install once
 * at a different base and fully remove it, then adopt a blob laid down by hand at
 * exec_base; unload must zero the ADOPTED base. */
static int test_adopt_remove_zeroes_blob(void)
{
	const uint64_t A = BLOB_BASE + 0x40000;
	uint64_t off;

	setup();
	g_ent.exec_base = A;
	g_ent.exec_bound = A + 0x4000;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);   /* g_base = A */
	kshim_remove();

	/* a valid adoptable blob at BLOB_BASE (!= A), laid down by hand */
	g_ent.exec_base = BLOB_BASE;
	g_ent.exec_bound = EXEC_BOUND;
	put_q(EVENT_SLOT, BLOB_BASE + DS45_SHIM_OFF_EVENT);
	put_q(ENABLE_SLOT, BLOB_BASE + DS45_SHIM_OFF_ENABLE);
	put_q(INPUT_SLOT, BLOB_BASE + DS45_SHIM_OFF_INPUT);
	put_q(BLOB_BASE, 0xC0DEC0DEC0DEC0DEULL);                   /* fake blob code */
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA, DS45_SHIM_MAGIC);
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, version),
	      ((uint64_t)DS45_SHIM_VERSION << 32) | 1);
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_applied), 0);

	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);     /* adopted at BLOB_BASE */

	kshim_unhook();
	kshim_scrub();

	/* the adopted base is the one scrubbed */
	for (off = 0; off < (uint64_t)DS45_SHIM_BLOB_LEN + 16; off += 8)
		CHECK(rd(BLOB_BASE + off) == 0);
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
	CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* With flush_desc == 0 the install writes no flush OPD and borrows nothing, and
 * unload zeroes only [base, base + blob): the 16 bytes past the blob are left
 * untouched (sentinel survives). */
static int test_install_no_flush_leaves_tail(void)
{
	const uint64_t sentinel = 0x5E5E5E5E5E5E5E5EULL;

	setup();
	g_ent.flush_desc = 0;
	put_q(BLOB_BASE + DS45_SHIM_BLOB_LEN, sentinel);

	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(g_spare_calls == 0);                                 /* nothing borrowed */
	CHECK(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN) == sentinel);     /* no flush OPD written */

	kshim_remove();
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == 0);            /* blob zeroed */
	CHECK(rd(BLOB_BASE + DS45_SHIM_BLOB_LEN) == sentinel);     /* tail untouched */
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* SHIM_INLINE_PID decides whether the two inline words go in. */
static int test_inline_rows_gated(void)
{
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q);
	kshim_remove();

	setup();
	g_cfg.shim_inline_pid = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(PID0_QA) == ((PID0_Q & HI32) | PID0_WORD));
	CHECK(rd(PID1_QA) == ((PID1_Q & HI32) | PID1_WORD));
	CHECK(rd(HELPER_QA) == ((HELPER_Q & HI32) | HELPER_WORD));
	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);
	kshim_remove();

	/* with the gate on, an inline site that is not as researched refuses the
	 * whole install; with it off the site is never looked at */
	setup();
	g_cfg.shim_inline_pid = 1;
	put_q(PID1_QA, 0xDEADBEEFDEADBEEFULL);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(g_spare_calls == 0);
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
	setup();
	put_q(PID1_QA, 0xDEADBEEFDEADBEEFULL);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(PID1_QA) == 0xDEADBEEFDEADBEEFULL);
	kshim_remove();
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A refusal before anything is written or borrowed: the image exactly as
 * found (the fixed blob region is scratch and not in the snap). */
static int refused_untouched(const struct snap *before, int pokes)
{
	struct snap after;

	CHECK_SUB(kshim_install_from(&g_ent, 1) == KSHIM_FAILED ? 0 : __LINE__);
	CHECK_SUB(fake_lv2_poke_count == pokes ? 0 : __LINE__);
	CHECK_SUB(g_spare_calls == 0 ? 0 : __LINE__);
	take_snap(&after);
	CHECK_SUB(snap_eq(before, &after) ? 0 : __LINE__);
	return 0;
}

/* SHIM_INPUT_HOOK gates the input redirect (0x37CE38). With it off the event,
 * enable and helper all install, but the input slot is left original and never
 * journalled (orig_input is still filled in the data block), so the shorter,
 * journal-driven unload restores the rest and leaves the input slot alone. With
 * it on the input slot is redirected to the blob, exactly as today. */
static int test_input_hook_gated(void)
{
	setup();
	g_cfg.shim_input_hook = 0;
	wlog_start();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	fake_lv2_on_peek = NULL;

	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);                       /* left original */
	CHECK(!wlog_has(INPUT_SLOT));                              /* never written */
	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);   /* the rest install */
	CHECK(rd(ENABLE_SLOT) == BLOB_BASE + DS45_SHIM_OFF_ENABLE);
	CHECK(rd(HELPER_QA) == ((HELPER_Q & HI32) | HELPER_WORD));
	/* the data block still records the input original the hooks would tail-call */
	CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA +
	         offsetof(ds45_shim_data_t, orig_input)) == INPUT_ORIG);

	/* the shorter journal reverts event, enable and helper; the input slot was
	 * never ours, so it is left exactly as found */
	kshim_remove();
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
	CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(HELPER_QA) == HELPER_Q);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* with the hook on (the default) the input slot is redirected to the blob */
	setup();
	g_cfg.shim_input_hook = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(INPUT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_INPUT);
	kshim_remove();
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_event_code_hash_mismatch(void)
{
	struct snap before;

	setup();
	g_ent.event_code_hash ^= 1;
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	/* the code itself changed (another kernel build) */
	setup();
	put_q(EVENT_CODE, EVENT_CODE_Q + 1);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	return 0;
}

static int test_enable_code_hash_mismatch(void)
{
	struct snap before;

	setup();
	g_ent.enable_code_hash ^= 0x100;
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	setup();
	put_q(ENABLE_CODE_QA, ENABLE_CODE_Q ^ 0xFF);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	return 0;
}

/* The code hashes are of the 8-aligned qword HOLDING the code address, never of
 * the 8 bytes starting at it. ENABLE_CODE is not 8-aligned (like the real
 * 0xCB234): a row carrying the hash of the 8 bytes from ENABLE_CODE (the low half
 * of its qword and the high half of the next) must be refused untouched. */
static int test_code_hash_is_of_the_aligned_qword(void)
{
	struct snap before;
	uint8_t b[8];
	uint64_t next = 0x9999AAAABBBBCCCCULL;
	int i;

	setup();
	put_q(ENABLE_CODE_QA + 8, next);
	for (i = 0; i < 4; i++) {
		b[i]     = (uint8_t)(ENABLE_CODE_Q >> (24 - 8 * i));
		b[4 + i] = (uint8_t)(next >> (56 - 8 * i));
	}
	g_ent.enable_code_hash = ds45_fnv1a64(b, 8);
	CHECK(g_ent.enable_code_hash != qhash(ENABLE_CODE_Q));
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	/* the aligned-qword hash of the same address installs */
	setup();
	put_q(ENABLE_CODE_QA + 8, next);
	CHECK((ENABLE_CODE & 7) != 0);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	kshim_remove();
	return 0;
}

static int test_enable_slot_mismatch(void)
{
	struct snap before;

	setup();
	put_q(ENABLE_SLOT, ENABLE_ORIG + 0x18);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	/* the event and input slots are checked the same way */
	setup();
	put_q(EVENT_SLOT, EVENT_ORIG + 0x18);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	setup();
	put_q(INPUT_SLOT, INPUT_ORIG + 0x18);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	/* and the helper site */
	setup();
	put_q(HELPER_QA, HELPER_Q ^ 1);
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);

	/* a row without a template is refused the same way */
	setup();
	g_ent.info_tmpl = NULL;
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	return 0;
}

/* case: a redirect slot changes between the verify and the write. */
static void toctou_on_peek(uint64_t addr)
{
	if (addr == INPUT_SLOT)
		put_q(INPUT_SLOT, TOCTOU_VAL);   /* a concurrent kernel write */
}

/* A redirect slot that changes between the step-2 verify and the step-8 apply
 * fails that apply's own verify, so nothing lasting is written and the image is
 * left as found (the blob copy to the fixed region is scratch, not in the snap). */
static int test_install_fails_on_toctou(void)
{
	struct snap before, after;

	setup();
	take_snap(&before);
	fake_lv2_on_peek = toctou_on_peek;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	fake_lv2_on_peek = NULL;
	CHECK(rd(INPUT_SLOT) == TOCTOU_VAL);   /* not clobbered */
	put_q(INPUT_SLOT, INPUT_ORIG);         /* undo the test's artefact */
	take_snap(&after);
	CHECK(snap_eq(&before, &after));
	CHECK(rd(SPARE_SLOT) == SPARE_STUB);   /* the borrowed slot is restored */

	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A write failure at each step leaves the image byte-identical: the blob copy,
 * the data block, and each write of the final apply (helper, [inline,] event,
 * enable, input). */
static int fail_at(int poke_index, int inline_pid)
{
	struct snap before, after;

	setup();
	g_cfg.shim_inline_pid = inline_pid;
	take_snap(&before);
	g_fault_at = poke_index;
	fake_lv2_poke_fail_after = g_fault_at;
	fake_lv2_on_peek = clear_fault_after_failed_poke;
	CHECK_SUB(kshim_install_from(&g_ent, 1) == KSHIM_FAILED ? 0 : __LINE__);
	fake_lv2_on_peek = NULL;
	fake_lv2_poke_fail_after = -1;
	take_snap(&after);
	CHECK_SUB(snap_eq(&before, &after) ? 0 : __LINE__);
	CHECK_SUB(rd(SPARE_SLOT) == SPARE_STUB ? 0 : __LINE__);
	CHECK_SUB(fake_lv2_out_of_bounds == 0 ? 0 : __LINE__);
	return 0;
}

static int test_rollback_each_step(void)
{
	int pokes, apply_start, i, g, n;
	uint32_t off;

	for (g = 0; g < 2; g++) {
		n = g ? 6 : 4;                 /* helper, [2 inline,] event, enable, input */

		/* calibrate on a clean install: remove writes back one qword per journal
		 * entry (the last writes the installer made) and then zeroes the fixed
		 * blob region (blob + flush OPD) */
		setup();
		g_cfg.shim_inline_pid = g;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		pokes = fake_lv2_poke_count;
		kshim_remove();
		CHECK(fake_lv2_poke_count - pokes == n + (DS45_SHIM_BLOB_LEN + 16) / 8);
		apply_start = pokes - n;

		for (i = 0; i < n; i++)
			CHECK(fail_at(apply_start + i, g) == 0);
	}

	/* the data block: its first qword the blob does not already hold (the blob
	 * ships magic/version; orig_event at +16 is zero in it). With no allocator
	 * the blob copy is the installer's first write, so the data block begins at
	 * poke BLOB_LEN/8. */
	CHECK(offsetof(ds45_shim_data_t, orig_event) == 16);
	CHECK(fail_at(DS45_SHIM_BLOB_LEN / 8 + 2, 0) == 0);

	/* a blob qword well into the copy whose written value is non-zero */
	for (off = 64; off < DS45_SHIM_OFF_DATA; off += 8)
		if (blob_q(off) != 0 || is_reloc(off))
			break;
	CHECK(off < DS45_SHIM_OFF_DATA);
	CHECK(fail_at((int)(off / 8), 0) == 0);
	return 0;
}

/* The ENABLE_SHIM gate. With the flag off - its default - the installer does
 * nothing at all: no read (not even the fingerprint), no write, nothing borrowed. */
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
	CHECK(g_spare_calls == 0);
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
	CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(HELPER_QA) == HELPER_Q);
	kshim_remove();                                  /* harmless */
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);

	/* the same row and the same image install as soon as the flag is set */
	g_cfg.enable_shim = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);
	kshim_remove();

	/* kshim_install(), the built-in-table entry point, is gated the same way */
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

static uint64_t db_q(size_t off)
{
	return rd(BLOB_BASE + DS45_SHIM_OFF_DATA + off);
}

/* A second install finds the event slot pointing at a blob (with the enable and
 * input slots at the same base) whose data block carries the magic and this
 * version: it adopts it and writes nothing. Adoption happens exactly when an
 * earlier load never ran remove, so removing the adopted install must undo
 * everything that load did: the three redirects and every product-ID word it
 * applied (rebuilt from the originals the install kept in the data block). */
static int test_adopt_via_event_slot(void)
{
	struct snap pristine, before, after;
	int pokes, g;

	for (g = 0; g < 2; g++) {
		setup();
		g_cfg.shim_inline_pid = g;
		take_snap(&pristine);
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

		/* the install recorded the originals of what it applied */
		CHECK(db_q(offsetof(ds45_shim_data_t, pid_orig)) == HELPER_Q);
		CHECK(db_q(offsetof(ds45_shim_data_t, pid_orig) + 8) == (g ? PID0_Q : 0));
		CHECK(db_q(offsetof(ds45_shim_data_t, pid_orig) + 16) == (g ? PID1_Q : 0));
		CHECK((uint32_t)(db_q(offsetof(ds45_shim_data_t, pid_applied)) >> 32) ==
		      (g ? 7u : 1u));

		/* the next load adopts it (whatever its own SHIM_INLINE_PID says: what
		 * gets restored is what the install applied) */
		g_cfg.shim_inline_pid = !g;
		g_spare_calls = 0;
		pokes = fake_lv2_poke_count;
		g_tr_applied_n = 0;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
		CHECK(fake_lv2_poke_count == pokes);   /* zero pokes */
		CHECK(g_spare_calls == 0);             /* nothing re-allocated */
		CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);
		/* the console log names the word edits the adopted install recorded */
		CHECK(g_tr_applied_n == 1 && g_tr_applied == (g ? 7u : 1u));

		/* removing the adopted install leaves the image as it was before the
		 * first install: redirects, helper word and (with g) the inline words */
		kshim_remove();
		take_snap(&after);
		CHECK(snap_eq(&pristine, &after));
		CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
		CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
		CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
		CHECK(rd(HELPER_QA) == HELPER_Q);
		CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q);
	}

	/* a blob whose enable slot is not at the event slot's base is not ours */
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	put_q(ENABLE_SLOT, ENABLE_ORIG);
	take_snap(&before);
	pokes = fake_lv2_poke_count;
	g_spare_calls = 0;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(fake_lv2_poke_count == pokes);
	take_snap(&after);
	CHECK(snap_eq(&before, &after));
	return 0;
}

/* Adoption trusts a recorded original only if it hashes to the row's expected
 * value AND the site still holds exactly that original with the row's word
 * applied. A site failing either check is left as found (not journalled), the
 * others - and the redirects - are still adopted and restored. */
static int test_adopt_skips_corrupt_orig(void)
{
	const uint64_t orig_at = BLOB_BASE + DS45_SHIM_OFF_DATA +
	                         offsetof(ds45_shim_data_t, pid_orig);
	uint64_t helper_patched, pid1_changed;
	int pokes;

	setup();
	g_cfg.shim_inline_pid = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	helper_patched = rd(HELPER_QA);
	CHECK(helper_patched == ((HELPER_Q & HI32) | HELPER_WORD));

	/* the helper's recorded original no longer hashes to the row's value */
	put_q(orig_at, HELPER_Q ^ 0x10);
	/* the second inline site was changed by someone else since the install
	 * (its high half), so it is not "original + our word" any more */
	pid1_changed = rd(PID1_QA) ^ 0x0100000000000000ULL;
	put_q(PID1_QA, pid1_changed);

	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);   /* still adopted */
	CHECK(fake_lv2_poke_count == pokes);

	kshim_remove();
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG);                     /* redirects restored */
	CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
	CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(PID0_QA) == PID0_Q);                            /* the good site too */
	CHECK(rd(HELPER_QA) == helper_patched);                  /* left as found */
	CHECK(rd(PID1_QA) == pid1_changed);                      /* left as found */

	/* a bit for a site the install did not apply, with garbage behind it, is
	 * not trusted either: nothing beyond the redirects is restored */
	setup();
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED); /* helper only */
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_applied),
	      (uint64_t)0xFFFFFFFFu << 32);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_remove();
	CHECK(rd(EVENT_SLOT) == EVENT_ORIG && rd(INPUT_SLOT) == INPUT_ORIG);
	CHECK(rd(HELPER_QA) == HELPER_Q);                        /* recorded: restored */
	CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q);   /* never touched */
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* A blob left by a build with a different data-block layout is NOT adopted. */
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
	CHECK(g_spare_calls == 0);             /* nothing borrowed */
	CHECK(rd(EVENT_SLOT) == BLOB_BASE + DS45_SHIM_OFF_EVENT);   /* left as found */

	/* the current version is adopted */
	put_q(BLOB_BASE + DS45_SHIM_OFF_DATA + 8,
	      ((uint64_t)DS45_SHIM_VERSION << 32) | 1);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_remove();
	return 0;
}

static int test_remove_restores_all(void)
{
	struct snap before, after;
	int pokes, g;

	for (g = 0; g < 2; g++) {
		setup();
		g_cfg.shim_inline_pid = g;
		take_snap(&before);
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		CHECK(rd(EVENT_SLOT) != EVENT_ORIG && rd(ENABLE_SLOT) != ENABLE_ORIG);
		CHECK(rd(INPUT_SLOT) != INPUT_ORIG && rd(HELPER_QA) != HELPER_Q);
		CHECK(g ? rd(PID0_QA) != PID0_Q : rd(PID0_QA) == PID0_Q);

		/* one revert per patch (helper, [two inline,] event, enable, input),
		 * then the fixed blob region is zeroed (blob + flush OPD) */
		pokes = fake_lv2_poke_count;
		kshim_remove();
		CHECK(fake_lv2_poke_count - pokes ==
		      (g ? 6 : 4) + (DS45_SHIM_BLOB_LEN + 16) / 8);
		take_snap(&after);
		CHECK(snap_eq(&before, &after));
		CHECK(rd(EVENT_SLOT) == EVENT_ORIG);
		CHECK(rd(ENABLE_SLOT) == ENABLE_ORIG);
		CHECK(rd(INPUT_SLOT) == INPUT_ORIG);
		CHECK(rd(HELPER_QA) == HELPER_Q);
		CHECK(rd(PID0_QA) == PID0_Q && rd(PID1_QA) == PID1_Q);

		/* the fixed blob region is zeroed on unload (it cannot be freed) */
		CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == 0);
		CHECK(rd(BLOB_BASE) == 0);

		/* remove again is a harmless no-op */
		pokes = fake_lv2_poke_count;
		kshim_remove();
		CHECK(fake_lv2_poke_count == pokes);
	}
	return 0;
}

/* A WORD row is a finished instruction: it lands verbatim whatever base the
 * blob is placed at, while the redirects follow that base. */
static int test_word_rows_do_not_follow_the_load_base(void)
{
	static const uint64_t base[2] = { BLOB_BASE, BLOB_BASE + 0x40000 };
	uint64_t h[2], ev[2];
	int i;

	for (i = 0; i < 2; i++) {
		setup();
		g_ent.exec_base = base[i];
		g_ent.exec_bound = base[i] + 0x4000;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		h[i] = rd(HELPER_QA);
		ev[i] = rd(EVENT_SLOT);
		kshim_remove();
	}
	CHECK(h[0] == ((HELPER_Q & HI32) | HELPER_WORD) && h[1] == h[0]);
	CHECK(ev[0] == base[0] + DS45_SHIM_OFF_EVENT);
	CHECK(ev[1] == base[1] + DS45_SHIM_OFF_EVENT);
	return 0;
}

/* The DS3 info block of the facts note, section C1.5 (Bluetooth variant). */
static const uint8_t c15_template[0x56] = {
	0x00, 0x05, 0x03, 0x50, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x8A, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFA,
	0x01, 0x8E, 0x02, 0x00, 0x01, 0x91, 0x02, 0x00,
	0x01, 0x8D, 0x01, 0xE6, 0x00, 0x00, 0x02, 0x6D,
	0x02, 0x70, 0x01, 0x01, 0x01, 0x00
};

/* The built-in table carries exactly the one researched DEX 4.92 row. */
static int test_builtin_table(void)
{
	const ds45_shim_kernel_t *k = &ds45_shim_table[0];
	int i;

	CHECK(ds45_shim_table_count == 1);
	CHECK(k->spare_syscall == 149u);

	/* the fixed executable region: the free zero tail 0x57C000..0x580000 */
	CHECK(k->exec_base == 0x800000000057C000ULL);
	CHECK(k->exec_bound == 0x8000000000580000ULL);

	CHECK(k->event_slot == 0x800000000037CFF8ULL);
	CHECK(k->event_expect == 0x800000000033A2B0ULL);
	CHECK(k->event_code_addr == 0x80000000000CACF0ULL);
	CHECK(k->event_code_hash == 0x16E42A0B1EBFEFFAULL);
	CHECK(k->enable_slot == 0x800000000037CFB0ULL);
	CHECK(k->enable_expect == 0x800000000033A310ULL);
	CHECK(k->enable_code_addr == 0x80000000000CB234ULL);
	/* FNV of the aligned qword 0xCB230 that holds 0xCB234 (hash_ok's rule), not
	 * of the 8 bytes from 0xCB234 */
	CHECK(k->enable_code_hash == 0xB52798EFCFE165A8ULL);
	CHECK(k->input_slot == 0x800000000037CE38ULL);
	CHECK(k->input_expect == 0x800000000033A010ULL);
	CHECK(k->pad_vtable == 0x800000000038CDC8ULL);
	CHECK(k->pad_mgr == 0x80000000004A1CB8ULL);

	/* the template is the facts note's C1.5 array, identity bytes zero */
	CHECK(k->info_tmpl != NULL);
	CHECK(memcmp(k->info_tmpl, c15_template, DS45_SHIM_INFO_LEN) == 0);
	for (i = 8; i < 16; i++)
		CHECK(k->info_tmpl[i] == 0);

	/* acceptance: the helper word always, the two inline words only with
	 * SHIM_INLINE_PID - all WORD rows, never a branch */
	CHECK(DS45_SHIM_PID_PATCHES == 1);
	CHECK(DS45_SHIM_INLINE_PATCHES == 2);
	CHECK(k->pid_patches[0].kind == DS45_KPATCH_WORD);
	CHECK(k->pid_patches[0].addr == 0x800000000029E2CCULL);
	CHECK(k->pid_patches[0].expect == 0x8D1047F832C76096ULL);
	CHECK(k->pid_patches[0].value == 0x68800CE6ULL);
	CHECK(k->inline_patches[0].kind == DS45_KPATCH_WORD);
	CHECK(k->inline_patches[0].addr == 0x800000000011B5E4ULL);
	CHECK(k->inline_patches[0].expect == 0xE8ADD0EDF3B2B3C1ULL);
	CHECK(k->inline_patches[0].value == 0x2F800CE6ULL);
	CHECK(k->inline_patches[1].kind == DS45_KPATCH_WORD);
	CHECK(k->inline_patches[1].addr == 0x800000000011B5ECULL);
	CHECK(k->inline_patches[1].expect == 0xF86140C9C3D7FB8DULL);
	CHECK(k->inline_patches[1].value == 0x4BFFFC48ULL);
	return 0;
}

/* ----- the built-in row against the pristine DEX 4.92 image -------------- */

/* The pristine LV2 code image (LV2 offset = file offset) is Sony's code, so it
 * lives outside the repository and never goes into git. It is found through the
 * environment variable DS45_LV2_492_IMAGE, else at <repo>/../analysis/
 * lv2_code_492.bin. Without it the image test prints SKIP and passes. */
#define IMAGE_492_REL "../analysis/lv2_code_492.bin"

/* Loads the image into the fake LV2 at DS45_LV2_BASE; its length, 0 if absent. */
static long load_image_492(void)
{
	char path[1024];
	const char *env = getenv("DS45_LV2_492_IMAGE");
	const char *file = __FILE__;   /* <repo>/tests/test_kshim.c */
	size_t len, cut = 0;
	int seps = 0;
	FILE *f;
	uint8_t *buf;
	long n;

	if (env && *env) {
		if (strlen(env) >= sizeof(path))
			return 0;
		strcpy(path, env);
	} else {
		for (len = strlen(file); len > 0; len--)
			if (file[len - 1] == '/' || file[len - 1] == '\\')
				if (++seps == 2) {
					cut = len;
					break;
				}
		if (seps < 2 || cut + sizeof(IMAGE_492_REL) > sizeof(path))
			return 0;
		memcpy(path, file, cut);
		memcpy(path + cut, IMAGE_492_REL, sizeof(IMAGE_492_REL));
	}

	f = fopen(path, "rb");
	if (!f)
		return 0;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
	    (uint64_t)n > DS45_LV2_BOUND - DS45_LV2_BASE || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return 0;
	}
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return 0;
	}
	fclose(f);
	fake_lv2_set(B, buf, (int)n);
	free(buf);
	return n;
}

/* In [B, B + n): an address the installer reads from the code image. */
static int in_image(uint64_t addr, long n)
{
	return addr >= B && addr + 8 <= B + (uint64_t)n;
}

/* The DEX 4.92 row, exactly as built in, against the pristine code image and
 * the installer's own hash rules: the 32-byte fingerprint, the event and enable
 * code qwords (the aligned qword holding each address) and every product-ID
 * site. The data slots past the code image are planted with the row's own
 * expected values (they are pointers the row states, not code). This is the test
 * that catches a row hash computed over the wrong bytes - for example over the 8
 * bytes starting at an unaligned code address instead of the qword holding it -
 * which no synthetic row can. Both SHIM_INLINE_PID settings; remove restores the
 * image. */
static int test_builtin_row_on_pristine_image(void)
{
	const ds45_shim_kernel_t *k = &ds45_shim_table[0];
	uint64_t spare = k->syscall_table + (uint64_t)k->spare_syscall * 8;
	uint64_t q[DS45_SHIM_PID_SITES], qa[DS45_SHIM_PID_SITES];
	long n;
	int inl, i;

	for (inl = 0; inl < 2; inl++) {
		setup();
		n = load_image_492();
		if (n == 0) {
			printf("test_kshim: SKIP built-in row vs pristine DEX 4.92 image "
			       "(not found; set DS45_LV2_492_IMAGE)\n");
			return 0;
		}

		/* what the installer hashes lies in the image ... */
		CHECK(in_image(k->fp_addr, n) && k->fp_addr + DS45_FP_LEN <= B + (uint64_t)n);
		CHECK(in_image(k->event_code_addr & ~7ULL, n));
		CHECK(in_image(k->enable_code_addr & ~7ULL, n));
		for (i = 0; i < DS45_SHIM_PID_SITES; i++) {
			qa[i] = (i < DS45_SHIM_PID_PATCHES ? k->pid_patches[i].addr
			         : k->inline_patches[i - DS45_SHIM_PID_PATCHES].addr) & ~7ULL;
			CHECK(in_image(qa[i], n));
			q[i] = rd(qa[i]);
		}
		/* ... and what is planted lies past it, so it masks no image byte; the
		 * fixed exec region (0x57C000..) is past the code image too, so it reads
		 * zero and the installer's zero-check passes */
		CHECK(!in_image(k->event_slot, n) && !in_image(k->enable_slot, n) &&
		      !in_image(k->input_slot, n) && !in_image(spare, n) &&
		      !in_image(k->alloc_desc, n) && k->exec_base >= B + (uint64_t)n);
		put_q(k->event_slot, k->event_expect);
		put_q(k->enable_slot, k->enable_expect);
		put_q(k->input_slot, k->input_expect);
		put_q(spare, k->spare_slot_expect);
		put_q(k->alloc_desc, ALLOC_ENTRY);
		put_q(k->alloc_desc + 8, KERNEL_TOC);
		fake_spare_syscall = (int)k->spare_syscall;
		g_cfg.shim_inline_pid = inl;

		CHECK(kshim_install_from(ds45_shim_table, ds45_shim_table_count) ==
		      KSHIM_INSTALLED);
		CHECK(rd(k->event_slot) == k->exec_base + DS45_SHIM_OFF_EVENT);
		CHECK(rd(k->enable_slot) == k->exec_base + DS45_SHIM_OFF_ENABLE);
		CHECK(rd(k->input_slot) == k->exec_base + DS45_SHIM_OFF_INPUT);
		CHECK(rd(qa[0]) != q[0]);
		CHECK((rd(qa[1]) != q[1]) == (inl != 0));
		CHECK((rd(qa[2]) != q[2]) == (inl != 0));

		kshim_remove();
		for (i = 0; i < DS45_SHIM_PID_SITES; i++)
			CHECK(rd(qa[i]) == q[i]);
		CHECK(rd(k->event_slot) == k->event_expect);
		CHECK(rd(k->enable_slot) == k->enable_expect);
		CHECK(rd(k->input_slot) == k->input_expect);
		CHECK(rd(spare) == k->spare_slot_expect);
		CHECK(fake_lv2_out_of_bounds == 0);
	}
	return 0;
}

int run_tests(void)
{
	int (*const tests[])(void) = {
		test_blob_tail_is_the_data_block,
		test_no_entry_writes_nothing,
		test_install_writes_v3,
		test_install_uses_exec_base,
		test_install_refuses_nonzero_region,
		test_install_refuses_over_bound,
		test_inline_rows_gated,
		test_input_hook_gated,
		test_event_code_hash_mismatch,
		test_enable_code_hash_mismatch,
		test_code_hash_is_of_the_aligned_qword,
		test_enable_slot_mismatch,
		test_install_fails_on_toctou,
		test_rollback_each_step,
		test_remove_zeroes_blob,
		test_remove_keeps_hooks_on_failed_revert,
		test_install_no_flush_leaves_tail,
		test_exec_probe_stub,
		test_probe_fault_each_step,
		test_install_is_gated_by_enable_shim,
		test_adopt_via_event_slot,
		test_adopt_skips_corrupt_orig,
		test_adopt_requires_the_same_version,
		test_adopt_remove_zeroes_blob,
		test_remove_restores_all,
		test_word_rows_do_not_follow_the_load_base,
		test_builtin_table,
		test_builtin_row_on_pristine_image,
	};
	unsigned i;
	int line;

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		line = tests[i]();
		if (line)
			return line;
	}
	return 0;
}
