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
#define CONN_TBL     0x8000000000432690ULL
/* The code address of the kernel's Bluetooth output sender (0x10283C on DEX
 * 4.92), which the output hook will tail-call; here only carried into the block. */
#define OUTPUT_ORIG  0x800000000010283CULL

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

/* Product-ID sites 3-6 (the output call site, the cap word and the two TOC
 * slots) are the row's output_patches[0..3]. The fixture row leaves them empty;
 * plant_output_rows() fills them, shaped like the DEX 4.92 row: [0] a BRANCHL at
 * the HIGH half of its qword, [1] the cap WORD at the LOW half, [2] and [3] PTR
 * rows on two TOC slots holding the old packet buffer address and that plus one.
 * The qwords are made-up patterns; every row has a distinct address so a site
 * index that reached the wrong row would patch the wrong qword. */
#define OUT_PATCHES_N 4
#define OUT_QA(i)    (B + 0x7100 + 8 * (i))
#define OUT_SITE(i)  (OUT_QA(i) + 4)
#define OUT_Q(i)     (0x7171717100000000ULL + (uint64_t)(i) * 0x0101010101ULL)
#define OUT_WORD(i)  (0x60000000u + (uint32_t)(i))
#define OUT_CALL_QA  OUT_QA(0)         /* bl: the high half (addr & 4 == 0) */
#define OUT_CAP_QA   OUT_QA(1)         /* the cap compare: the low half */
#define OUT_CAP_SITE (OUT_CAP_QA + 4)
#define OUT_SLOT5    (B + 0x7200)      /* TOC slot holding the old buffer address */
#define OUT_SLOT6    (B + 0x7208)      /* ... and that plus one */
#define OLD_BUF      0x80000000004326C8ULL
#define CAP_WORD     0x2B9F004Du       /* cmplwi cr7,r31,0x4D */

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
	g_cfg.shim_stable_id = 1;    /* the stable identity is on by default */

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
	g_ent.conn_tbl = CONN_TBL;
	g_ent.orig_output = OUTPUT_ORIG;
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
                       ecode, ncode, call, cap, slot5, slot6; };

/* Fill the row's four output patches (data-block sites 3-6) and plant the qwords
 * they were researched against. A test that wants the output sites applied sets
 * g_cfg.shim_output_hook (and shim_output_mode) after this. */
static void plant_output_rows(void)
{
	g_ent.output_patches[0].kind = DS45_KPATCH_BRANCHL;
	g_ent.output_patches[0].addr = OUT_CALL_QA;
	g_ent.output_patches[0].expect = qhash(OUT_Q(0));
	g_ent.output_patches[0].value = 0;                 /* filled in at install */
	g_ent.output_patches[1].kind = DS45_KPATCH_WORD;
	g_ent.output_patches[1].addr = OUT_CAP_SITE;
	g_ent.output_patches[1].expect = qhash(OUT_Q(1));
	g_ent.output_patches[1].value = CAP_WORD;
	g_ent.output_patches[2].kind = DS45_KPATCH_PTR;
	g_ent.output_patches[2].addr = OUT_SLOT5;
	g_ent.output_patches[2].expect = OLD_BUF;          /* PTR rows compare the value */
	g_ent.output_patches[2].value = 0;
	g_ent.output_patches[3].kind = DS45_KPATCH_PTR;
	g_ent.output_patches[3].addr = OUT_SLOT6;
	g_ent.output_patches[3].expect = OLD_BUF + 1;
	g_ent.output_patches[3].value = 0;
	put_q(OUT_CALL_QA, OUT_Q(0));
	put_q(OUT_CAP_QA, OUT_Q(1));
	put_q(OUT_SLOT5, OLD_BUF);
	put_q(OUT_SLOT6, OLD_BUF + 1);
}

/* The packet buffer in a blob loaded at base. */
static uint64_t pktbuf_at(uint64_t base)
{
	return base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pktbuf);
}

/* "bl target" as PowerPC encodes it (AA = 0, LK = 1), worked out here
 * independently of kpatch_branchl_word. */
static uint32_t ppc_bl(uint64_t from, uint64_t to)
{
	return 0x48000001u | (uint32_t)((to - from) & 0x03FFFFFCu);
}

static void take_snap(struct snap *s)
{
	s->call    = rd(OUT_CALL_QA);
	s->cap     = rd(OUT_CAP_QA);
	s->slot5   = rd(OUT_SLOT5);
	s->slot6   = rd(OUT_SLOT6);
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
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, version)) == 9 ? 0 : __LINE__);
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
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, conn_tbl)) ==
	          CONN_TBL ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, stable_id)) == 1 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_id_bd)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_id_fallback)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_last_handle)) == 0 ? 0 : __LINE__);
	/* the DS3-route output fields: the sender's address from the row, the mode
	 * from the config (setup leaves it 0), everything else cleared */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, orig_output)) ==
	          OUTPUT_ORIG ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, out_mode)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, out_seq)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_out_calls)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_out_cap_len)) == 0 ? 0 : __LINE__);
	for (i = 0; i < 64; i++)
		CHECK_SUB(db[offsetof(ds45_shim_data_t, dbg_out_cap) + i] == 0 ? 0 : __LINE__);
	for (i = 0; i < 80; i++)
		CHECK_SUB(db[offsetof(ds45_shim_data_t, out_buf) + i] == 0 ? 0 : __LINE__);
	for (i = 0; i < 96; i++)
		CHECK_SUB(db[offsetof(ds45_shim_data_t, pktbuf) + i] == 0 ? 0 : __LINE__);
	/* the rumble-aware capture (debug only) starts cleared */
	CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, dbg_out_on_pad)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_out_on_count)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_out_off_count)) == 0 ? 0 : __LINE__);
	CHECK_SUB(be32b(db + offsetof(ds45_shim_data_t, dbg_out_off_armed)) == 0 ? 0 : __LINE__);
	for (i = 0; i < 64; i++) {
		CHECK_SUB(db[offsetof(ds45_shim_data_t, dbg_out_on_cap) + i] == 0 ? 0 : __LINE__);
		CHECK_SUB(db[offsetof(ds45_shim_data_t, dbg_out_off_cap) + i] == 0 ? 0 : __LINE__);
	}

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
	for (i = 3; i < DS45_SHIM_PID_SITES; i++)       /* the output sites: not enabled */
		CHECK_SUB(be64b(db + offsetof(ds45_shim_data_t, pid_orig) + 8 * i) == 0 ? 0 : __LINE__);
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

/* SHIM_STABLE_ID reaches the blob: off writes stable_id = 0 (the counter form
 * is then used), and the connection-table address is written either way. */
static int test_stable_id_switch(void)
{
	const uint64_t db = BLOB_BASE + DS45_SHIM_OFF_DATA;

	setup();
	g_cfg.shim_stable_id = 0;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK((rd(db + offsetof(ds45_shim_data_t, stable_id)) >> 32) == 0);
	CHECK(rd(db + offsetof(ds45_shim_data_t, conn_tbl)) == CONN_TBL);
	kshim_remove();

	setup();
	g_cfg.shim_stable_id = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK((rd(db + offsetof(ds45_shim_data_t, stable_id)) >> 32) == 1);
	kshim_remove();
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* SHIM_OUTPUT_MODE reaches the blob as out_mode, only for 0..2 (the parser
 * clamps; the block builder must not trust it), and the sender's address comes
 * from the firmware row. */
static int test_output_fields(void)
{
	const uint64_t db = BLOB_BASE + DS45_SHIM_OFF_DATA;
	static const int modes[] = { 0, 1, 2, 3, -1, 99 };
	static const uint32_t want[] = { 0, 1, 2, 0, 0, 0 };
	size_t m;

	for (m = 0; m < sizeof modes / sizeof modes[0]; m++) {
		setup();
		g_cfg.shim_output_mode = modes[m];
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		CHECK((rd(db + offsetof(ds45_shim_data_t, out_mode)) >> 32) == want[m]);
		CHECK(rd(db + offsetof(ds45_shim_data_t, orig_output)) == OUTPUT_ORIG);
		kshim_remove();
	}

	/* a row without the output sender (the built-in row until it is filled)
	 * writes a zero there */
	setup();
	g_ent.orig_output = 0;
	g_cfg.shim_output_mode = 2;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(rd(db + offsetof(ds45_shim_data_t, orig_output)) == 0);
	CHECK((rd(db + offsetof(ds45_shim_data_t, out_mode)) >> 32) == 2);
	kshim_remove();
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* With SHIM_OUTPUT_HOOK off the output sites (3-6) are never selected: a row full
 * of output patches installs exactly like one without - the same number of
 * writes, the same pid_applied bits (version 7's), zero pid_orig past site 2, the
 * output qwords never written - in both SHIM_INLINE_PID settings, and whatever
 * the output mode. (This test used to also forge an applied bit for site 3 and
 * expect adoption to ignore it; adoption now restores what the install recorded
 * whatever the config says, so that case lives in test_output_adopt_v8.) */
static int test_output_sites_hook_off(void)
{
	const uint64_t orig_at = BLOB_BASE + DS45_SHIM_OFF_DATA +
	                         offsetof(ds45_shim_data_t, pid_orig);
	const uint64_t applied_at = BLOB_BASE + DS45_SHIM_OFF_DATA +
	                            offsetof(ds45_shim_data_t, pid_applied);
	int g, i, mode, pokes, bare_pokes;

	for (g = 0; g < 2; g++) {
		/* the baseline: the same install with no output rows at all */
		setup();
		g_cfg.shim_inline_pid = g;
		pokes = fake_lv2_poke_count;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
		bare_pokes = fake_lv2_poke_count - pokes;
		kshim_remove();

		for (mode = 0; mode < 3; mode++) {
			setup();
			plant_output_rows();
			g_cfg.shim_inline_pid = g;
			g_cfg.shim_output_hook = 0;
			g_cfg.shim_output_mode = mode;
			wlog_start();
			pokes = fake_lv2_poke_count;
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
			fake_lv2_on_peek = NULL;
			CHECK(fake_lv2_poke_count - pokes == bare_pokes);
			CHECK(rd(OUT_CALL_QA) == OUT_Q(0) && rd(OUT_CAP_QA) == OUT_Q(1));
			CHECK(rd(OUT_SLOT5) == OLD_BUF && rd(OUT_SLOT6) == OLD_BUF + 1);
			for (i = 0; i < OUT_PATCHES_N; i++) {
				CHECK(!wlog_has(OUT_QA(i)));
				CHECK(rd(orig_at + 8 * (3 + i)) == 0);
			}
			CHECK(!wlog_has(OUT_SLOT5) && !wlog_has(OUT_SLOT6));
			CHECK((rd(applied_at) >> 32) == (g ? 7u : 1u));
			/* the output sender's address and the mode still reach the block */
			CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA +
			         offsetof(ds45_shim_data_t, orig_output)) == OUTPUT_ORIG);
			kshim_remove();
			CHECK(fake_lv2_out_of_bounds == 0);
		}
	}
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

/* "b target" as PowerPC encodes it (AA = 0, LK = 0), worked out here
 * independently of kpatch_branch_word. */
static uint32_t ppc_b(uint64_t from, uint64_t to)
{
	return 0x48000000u | (uint32_t)((to - from) & 0x03FFFFFCu);
}

/* Stage 1b: the helper row is a BRANCH to shim_pid_ok in the blob (a leaf that
 * accepts 0x0268, 0x042F and 0x0CE6), so the Move navigation controller keeps its
 * pad object. The branch follows the blob's load base, is verified against the
 * row's hash like a word edit, adopted and reverted like one. */
static int test_helper_branch_row(void)
{
	static const uint64_t base[2] = { BLOB_BASE, BLOB_BASE + 0x40000 };
	struct snap pristine, after;
	uint64_t h[2];
	int i;

	for (i = 0; i < 2; i++) {
		setup();
		g_ent.pid_patches[0].kind = DS45_KPATCH_BRANCH;
		g_ent.pid_patches[0].value = 0;
		g_ent.exec_base = base[i];
		g_ent.exec_bound = base[i] + 0x4000;
		take_snap(&pristine);
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

		/* the instruction in the helper's qword is now a branch to shim_pid_ok in
		 * the blob; the other half of the qword is untouched */
		h[i] = rd(HELPER_QA);
		CHECK((h[i] & 0xFFFFFFFFULL) ==
		      ppc_b(HELPER_SITE, base[i] + DS45_SHIM_OFF_PID_OK));
		CHECK((h[i] & HI32) == (HELPER_Q & HI32));

		/* the recorded original is the helper's qword, and only that site is live */
		CHECK(rd(base[i] + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_orig)) ==
		      HELPER_Q);
		CHECK((uint32_t)(rd(base[i] + DS45_SHIM_OFF_DATA +
		                    offsetof(ds45_shim_data_t, pid_applied)) >> 32) == 1u);

		/* the next load adopts it, and removing the adopted install restores the
		 * helper exactly (a branch row must adopt like a word row) */
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
		CHECK(rd(HELPER_QA) == h[i]);
		kshim_remove();
		take_snap(&after);
		CHECK(snap_eq(&pristine, &after));
		CHECK(rd(HELPER_QA) == HELPER_Q);
	}
	/* a different load base gives a different branch */
	CHECK(h[0] != h[1]);

	/* a helper that no longer holds the researched qword is refused, nothing written */
	setup();
	g_ent.pid_patches[0].kind = DS45_KPATCH_BRANCH;
	g_ent.pid_patches[0].value = 0;
	put_q(HELPER_QA, HELPER_Q ^ 1);
	take_snap(&pristine);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	take_snap(&after);
	CHECK(snap_eq(&pristine, &after));
	return 0;
}

/* ----- the DS3-route output sites (data-block sites 3-6) ------------------ */

/* The four output sites as they stand after an install at base: the call site a
 * bl to the blob's output entry (the high half of its qword; the low half kept)
 * or untouched, the cap word the low half of its qword or untouched, and both TOC
 * slots on the blob's packet buffer (and that plus one) or untouched. */
static int check_out_sites(uint64_t base, int call, int cap, int buf)
{
	const uint64_t bl = ((uint64_t)ppc_bl(OUT_CALL_QA, base + DS45_SHIM_OFF_OUTPUT) << 32) |
	                    (OUT_Q(0) & 0xFFFFFFFFULL);

	CHECK_SUB(rd(OUT_CALL_QA) == (call ? bl : OUT_Q(0)) ? 0 : __LINE__);
	CHECK_SUB(rd(OUT_CAP_QA) == (cap ? ((OUT_Q(1) & HI32) | CAP_WORD) : OUT_Q(1))
	          ? 0 : __LINE__);
	CHECK_SUB(rd(OUT_SLOT5) == (buf ? pktbuf_at(base) : OLD_BUF) ? 0 : __LINE__);
	CHECK_SUB(rd(OUT_SLOT6) == (buf ? pktbuf_at(base) + 1 : OLD_BUF + 1) ? 0 : __LINE__);
	return 0;
}

static uint64_t pid_orig_at(uint64_t base, int site)
{
	return rd(base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_orig) +
	          8 * (uint64_t)site);
}

static uint32_t pid_applied_at(uint64_t base)
{
	return (uint32_t)(rd(base + DS45_SHIM_OFF_DATA +
	                     offsetof(ds45_shim_data_t, pid_applied)) >> 32);
}

/* Make the memory behind output row k (0..3) differ from what the row expects. */
static void tamper_out(int k)
{
	switch (k) {
	case 0: put_q(OUT_CALL_QA, OUT_Q(0) ^ 1); break;
	case 1: put_q(OUT_CAP_QA, OUT_Q(1) ^ 1); break;
	case 2: put_q(OUT_SLOT5, OLD_BUF + 0x10); break;
	default: put_q(OUT_SLOT6, OLD_BUF + 0x11); break;
	}
}

/* Which output sites a configuration selects (SHIM_OUTPUT_HOOK / _MODE, and the
 * row's orig_output, since the stub tail-branches to it): sites 3, 5 and 6 with
 * the hook on in any mode, site 4 (the cap word) only in probe and full mode, and
 * none at all when the row has no sender address. An out-of-range mode is
 * observe, exactly as the block's out_mode treats it. For each case the install
 * must leave exactly those sites patched, record exactly their bits and
 * originals, journal exactly those writes, and unload must restore the image. */
static int test_output_sites_gated(void)
{
	static const struct {
		int hook, mode;
		uint64_t orig_output;
		int call, cap, buf;
	} c[] = {
		/* hook off: nothing beyond version 7, whatever the mode */
		{ 0, 0, OUTPUT_ORIG, 0, 0, 0 },
		{ 0, 1, OUTPUT_ORIG, 0, 0, 0 },
		{ 0, 2, OUTPUT_ORIG, 0, 0, 0 },
		/* hook on: observe takes the call site and the buffer slots ... */
		{ 1, 0, OUTPUT_ORIG, 1, 0, 1 },
		/* ... probe and full add the cap word */
		{ 1, 1, OUTPUT_ORIG, 1, 1, 1 },
		{ 1, 2, OUTPUT_ORIG, 1, 1, 1 },
		/* out-of-range modes are observe */
		{ 1, 3, OUTPUT_ORIG, 1, 0, 1 },
		{ 1, -1, OUTPUT_ORIG, 1, 0, 1 },
		{ 1, 99, OUTPUT_ORIG, 1, 0, 1 },
		/* no sender address in the row: the stub would branch to 0, so no output
		 * site is touched, whatever the config */
		{ 1, 0, 0, 0, 0, 0 },
		{ 1, 1, 0, 0, 0, 0 },
		{ 1, 2, 0, 0, 0, 0 },
		{ 0, 1, 0, 0, 0, 0 },
	};
	struct snap before, after;
	uint32_t want_mask;
	size_t t;
	int g, pokes, n;

	for (t = 0; t < sizeof c / sizeof c[0]; t++)
		for (g = 0; g < 2; g++) {
			setup();
			plant_output_rows();
			g_ent.orig_output = c[t].orig_output;
			g_cfg.shim_inline_pid = g;
			g_cfg.shim_output_hook = c[t].hook;
			g_cfg.shim_output_mode = c[t].mode;
			take_snap(&before);

			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
			CHECK(check_out_sites(BLOB_BASE, c[t].call, c[t].cap, c[t].buf) == 0);
			want_mask = 1u | (g ? 6u : 0u) | (c[t].call ? 8u : 0u) |
			            (c[t].cap ? 16u : 0u) | (c[t].buf ? 96u : 0u);
			CHECK(pid_applied_at(BLOB_BASE) == want_mask);
			CHECK(pid_orig_at(BLOB_BASE, 3) == (c[t].call ? OUT_Q(0) : 0));
			CHECK(pid_orig_at(BLOB_BASE, 4) == (c[t].cap ? OUT_Q(1) : 0));
			CHECK(pid_orig_at(BLOB_BASE, 5) == (c[t].buf ? OLD_BUF : 0));
			CHECK(pid_orig_at(BLOB_BASE, 6) == (c[t].buf ? OLD_BUF + 1 : 0));

			/* the journal: helper, [2 inline,] the output sites, 3 redirects - one
			 * qword write each on unload */
			n = 1 + (g ? 2 : 0) + c[t].call + c[t].cap + 2 * c[t].buf + 3;
			pokes = fake_lv2_poke_count;
			kshim_unhook();
			CHECK(fake_lv2_poke_count - pokes == n);
			kshim_scrub();
			take_snap(&after);
			CHECK(snap_eq(&before, &after));
			CHECK(fake_lv2_out_of_bounds == 0);
		}
	return 0;
}

/* Site index -> row: sites 3, 4, 5, 6 are the row's output_patches[0..3], each
 * with its own address, so a site that reached the wrong row would read, verify
 * or patch the wrong qword. With all four selected, each row's own qword changes
 * to its own value and its own original is recorded at its own index; tampering
 * with row k's memory (and only that) refuses the install, and with the cap row
 * unselected (observe mode) tampering with it is never noticed. */
static int test_output_site_index_mapping(void)
{
	int k;

	setup();
	plant_output_rows();
	g_cfg.shim_inline_pid = 1;
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(check_out_sites(BLOB_BASE, 1, 1, 1) == 0);
	CHECK(pid_applied_at(BLOB_BASE) == 0x7Fu);
	CHECK(pid_orig_at(BLOB_BASE, 0) == HELPER_Q);
	CHECK(pid_orig_at(BLOB_BASE, 1) == PID0_Q && pid_orig_at(BLOB_BASE, 2) == PID1_Q);
	CHECK(pid_orig_at(BLOB_BASE, 3) == OUT_Q(0));      /* output_patches[0] */
	CHECK(pid_orig_at(BLOB_BASE, 4) == OUT_Q(1));      /* [1] */
	CHECK(pid_orig_at(BLOB_BASE, 5) == OLD_BUF);       /* [2] */
	CHECK(pid_orig_at(BLOB_BASE, 6) == OLD_BUF + 1);   /* [3] */
	kshim_remove();

	for (k = 0; k < OUT_PATCHES_N; k++) {
		struct snap before;

		setup();
		plant_output_rows();
		g_cfg.shim_output_hook = 1;
		g_cfg.shim_output_mode = 1;
		tamper_out(k);
		take_snap(&before);
		CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	}

	/* observe mode: only the cap row is out of the picture */
	for (k = 0; k < OUT_PATCHES_N; k++) {
		setup();
		plant_output_rows();
		g_cfg.shim_output_hook = 1;
		g_cfg.shim_output_mode = 0;
		tamper_out(k);
		if (k == 1) {
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
			CHECK(rd(OUT_CAP_QA) == (OUT_Q(1) ^ 1));       /* never looked at */
			CHECK(pid_orig_at(BLOB_BASE, 4) == 0);
			kshim_remove();
			CHECK(rd(OUT_CAP_QA) == (OUT_Q(1) ^ 1));
		} else {
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
		}
	}
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* The call site is a bl to the blob's output entry, so it follows the load base
 * (a WORD row does not): same site, two bases, two encodings - each worked out
 * independently here - and an adopted install at the second base restores it. */
static int test_output_branchl_follows_load_base(void)
{
	static const uint64_t base[2] = { BLOB_BASE, BLOB_BASE + 0x40000 };
	struct snap pristine, after;
	uint64_t h[2];
	int i;

	for (i = 0; i < 2; i++) {
		setup();
		plant_output_rows();
		g_ent.exec_base = base[i];
		g_ent.exec_bound = base[i] + 0x4000;
		g_cfg.shim_output_hook = 1;
		g_cfg.shim_output_mode = 1;
		take_snap(&pristine);
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

		h[i] = rd(OUT_CALL_QA);
		CHECK((uint32_t)(h[i] >> 32) ==
		      ppc_bl(OUT_CALL_QA, base[i] + DS45_SHIM_OFF_OUTPUT));
		CHECK((h[i] & 0xFFFFFFFFULL) == (OUT_Q(0) & 0xFFFFFFFFULL));   /* low half kept */
		CHECK(check_out_sites(base[i], 1, 1, 1) == 0);
		CHECK(pid_orig_at(base[i], 3) == OUT_Q(0));

		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);     /* adopted */
		CHECK(rd(OUT_CALL_QA) == h[i]);
		kshim_remove();
		take_snap(&after);
		CHECK(snap_eq(&pristine, &after));
		CHECK(rd(OUT_CALL_QA) == OUT_Q(0));
	}
	CHECK(h[0] != h[1]);
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* The two TOC slots are redirected into the blob's own packet buffer: one holds
 * its address, the other that plus one, at any load base and in observe as well
 * as probe mode; the buffer lies inside the installed data block, 8-aligned with
 * room for its 96 bytes; unload puts the old values back. */
static int test_buffer_slots_point_into_blob(void)
{
	static const uint64_t base[2] = { BLOB_BASE, BLOB_BASE + 0x40000 };
	uint64_t pb[2], off;
	int i, mode;

	for (i = 0; i < 2; i++)
		for (mode = 0; mode < 2; mode++) {
			setup();
			plant_output_rows();
			g_ent.exec_base = base[i];
			g_ent.exec_bound = base[i] + 0x4000;
			g_cfg.shim_output_hook = 1;
			g_cfg.shim_output_mode = mode;
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);

			pb[i] = pktbuf_at(base[i]);
			CHECK(rd(OUT_SLOT5) == pb[i]);
			CHECK(rd(OUT_SLOT6) == pb[i] + 1);
			CHECK((pb[i] & 7) == 0);
			CHECK(pb[i] >= base[i] + DS45_SHIM_OFF_DATA);
			CHECK(pb[i] + sizeof(((ds45_shim_data_t *)0)->pktbuf) <=
			      base[i] + DS45_SHIM_BLOB_LEN);

			kshim_remove();
			CHECK(rd(OUT_SLOT5) == OLD_BUF);
			CHECK(rd(OUT_SLOT6) == OLD_BUF + 1);
			for (off = 0; off < (uint64_t)DS45_SHIM_BLOB_LEN + 16; off += 8)
				CHECK(rd(base[i] + off) == 0);
		}
	CHECK(pb[0] != pb[1]);                    /* the slots follow the load base */
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

/* Any one output site not holding what it was researched against fails the whole
 * install, before anything is written: the call site and the cap word by hash,
 * the two TOC slots by exact value. Statically (zero pokes, nothing borrowed) and
 * as a change between the verify and the apply (nothing lasting written: every
 * kernel qword is as found once the change is undone). */
static uint64_t g_toctou_addr, g_toctou_val;
static void toctou_out_on_peek(uint64_t addr)
{
	if (addr == g_toctou_addr)
		put_q(addr, g_toctou_val);
}

static int test_output_site_mismatch_fails_clean(void)
{
	static const uint64_t addr[OUT_PATCHES_N] =
		{ OUT_CALL_QA, OUT_CAP_QA, OUT_SLOT5, OUT_SLOT6 };
	static const uint64_t found[OUT_PATCHES_N] =
		{ OUT_Q(0), OUT_Q(1), OLD_BUF, OLD_BUF + 1 };
	struct snap before, after;
	int k, mode;

	for (mode = 1; mode < 3; mode++)
		for (k = 0; k < OUT_PATCHES_N; k++) {
			/* static mismatch */
			setup();
			plant_output_rows();
			g_cfg.shim_output_hook = 1;
			g_cfg.shim_output_mode = mode;
			tamper_out(k);
			take_snap(&before);
			CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
			CHECK(fake_lv2_out_of_bounds == 0);

			/* the site changes after it was verified, before the apply */
			setup();
			plant_output_rows();
			g_cfg.shim_output_hook = 1;
			g_cfg.shim_output_mode = mode;
			take_snap(&before);
			g_toctou_addr = addr[k];
			g_toctou_val = found[k] ^ 0x10;
			fake_lv2_on_peek = toctou_out_on_peek;
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
			fake_lv2_on_peek = NULL;
			CHECK(rd(addr[k]) == g_toctou_val);          /* not clobbered */
			put_q(addr[k], found[k]);                    /* undo the test's artefact */
			take_snap(&after);
			CHECK(snap_eq(&before, &after));
			CHECK(rd(SPARE_SLOT) == SPARE_STUB);
			CHECK(fake_lv2_out_of_bounds == 0);
		}
	return 0;
}

/* The call site's bl must reach the blob's output entry (a 26-bit displacement,
 * +-32 MiB) or the install is refused before the blob is even written. Within
 * the 8 MiB LV2 window every site reaches every base, so the range cannot fail
 * for a real row (asserted below); the check is still real code, and the same
 * step refuses a displacement that cannot be encoded - here a call site that is
 * not 4-aligned - with nothing written, borrowed or changed. */
static int test_output_branchl_out_of_range_refused(void)
{
	struct snap before;

	CHECK(DS45_LV2_BOUND - DS45_LV2_BASE <= 0x2000000ULL);    /* window within reach */

	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	g_ent.output_patches[0].addr = OUT_CALL_QA + 2;   /* its qword still hashes right */
	take_snap(&before);
	CHECK(refused_untouched(&before, fake_lv2_poke_count) == 0);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* the same row at an encodable address installs */
	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	kshim_remove();
	return 0;
}

/* One poke failing at each step of the final apply, with every output site and
 * the inline words selected, leaves the image byte-identical (the output sites
 * included in the snapshot). */
static int fail_at_out(int poke_index)
{
	struct snap before, after;

	setup();
	plant_output_rows();
	g_cfg.shim_inline_pid = 1;
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
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

static int test_output_rollback_each_step(void)
{
	const int n = 3 + 4 + 3;      /* helper + 2 inline, the 4 output sites, 3 redirects */
	int pokes, i;

	setup();
	plant_output_rows();
	g_cfg.shim_inline_pid = 1;
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	pokes = fake_lv2_poke_count;
	kshim_remove();
	CHECK(fake_lv2_poke_count - pokes == n + (DS45_SHIM_BLOB_LEN + 16) / 8);
	for (i = 0; i < n; i++)
		CHECK(fail_at_out(pokes - n + i) == 0);
	return 0;
}

/* The transaction's order, and the data block complete before the first patch.
 * Order: the helper and inline rows as today, then the two buffer slots back to
 * back, then the call site, then the cap word, then the event, enable and input
 * redirects. At each of those writes the block already carries the magic, the
 * version, the sender's address (orig_output: the stub tail-branches to it the
 * moment the call site is patched) and the recorded originals. */
static int g_ord_in_cb, g_ord_checks, g_ord_bad;
static void order_on_peek(uint64_t addr)
{
	static const uint64_t sites[] = {
		HELPER_QA, PID0_QA, PID1_QA, OUT_SLOT5, OUT_SLOT6, OUT_CALL_QA,
		OUT_CAP_QA, EVENT_SLOT, ENABLE_SLOT, INPUT_SLOT
	};
	const uint64_t db = BLOB_BASE + DS45_SHIM_OFF_DATA;
	int n0, i;

	if (g_ord_in_cb)
		return;
	n0 = g_wlog_n;
	wlog_on_peek(addr);
	if (g_wlog_n == n0)
		return;                                   /* not the read-back of a write */
	for (i = 0; i < (int)(sizeof sites / sizeof sites[0]); i++)
		if (sites[i] == addr) {
			g_ord_in_cb = 1;
			g_ord_checks++;
			if (rd(db) != DS45_SHIM_MAGIC ||
			    (uint32_t)(rd(db + offsetof(ds45_shim_data_t, version)) >> 32) !=
			        DS45_SHIM_VERSION ||
			    rd(db + offsetof(ds45_shim_data_t, orig_output)) != OUTPUT_ORIG ||
			    (uint32_t)(rd(db + offsetof(ds45_shim_data_t, pid_applied)) >> 32) != 0x7Fu ||
			    rd(db + offsetof(ds45_shim_data_t, pid_orig) + 8 * 3) != OUT_Q(0) ||
			    rd(db + offsetof(ds45_shim_data_t, pid_orig) + 8 * 5) != OLD_BUF)
				g_ord_bad++;
			g_ord_in_cb = 0;
		}
}

static int wlog_index(uint64_t addr)
{
	int i;
	for (i = 0; i < g_wlog_n; i++)
		if (g_wlog[i] == addr)
			return i;
	return -1;
}

static int test_output_apply_order_and_block_complete(void)
{
	int helper, p0, p1, s5, s6, call, cap, ev, en, in;

	setup();
	plant_output_rows();
	g_cfg.shim_inline_pid = 1;
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	wlog_start();
	g_ord_checks = g_ord_bad = g_ord_in_cb = 0;
	fake_lv2_on_peek = order_on_peek;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	fake_lv2_on_peek = NULL;

	CHECK(g_ord_checks == 10);                    /* every patched site was checked */
	CHECK(g_ord_bad == 0);

	helper = wlog_index(HELPER_QA);
	p0 = wlog_index(PID0_QA);
	p1 = wlog_index(PID1_QA);
	s5 = wlog_index(OUT_SLOT5);
	s6 = wlog_index(OUT_SLOT6);
	call = wlog_index(OUT_CALL_QA);
	cap = wlog_index(OUT_CAP_QA);
	ev = wlog_index(EVENT_SLOT);
	en = wlog_index(ENABLE_SLOT);
	in = wlog_index(INPUT_SLOT);
	CHECK(helper >= 0 && helper < p0 && p0 < p1 && p1 < s5);
	CHECK(s6 == s5 + 1);                          /* the two slots back to back */
	CHECK(s6 < call && call < cap && cap < ev && ev < en && en < in);
	CHECK(in == g_wlog_n - 1);                    /* the input redirect is the last write */
	kshim_remove();
	return 0;
}

/* Remove undoes every output site, in the same journal as the rest. */
static int test_output_remove_restores_all(void)
{
	struct snap before, after;
	int pokes, g, mode, n;

	for (g = 0; g < 2; g++)
		for (mode = 0; mode < 3; mode++) {
			setup();
			plant_output_rows();
			g_cfg.shim_inline_pid = g;
			g_cfg.shim_output_hook = 1;
			g_cfg.shim_output_mode = mode;
			take_snap(&before);
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
			CHECK(check_out_sites(BLOB_BASE, 1, mode != 0, 1) == 0);

			n = 1 + (g ? 2 : 0) + 1 + (mode != 0) + 2 + 3;
			pokes = fake_lv2_poke_count;
			kshim_remove();
			CHECK(fake_lv2_poke_count - pokes == n + (DS45_SHIM_BLOB_LEN + 16) / 8);
			take_snap(&after);
			CHECK(snap_eq(&before, &after));
			CHECK(check_out_sites(BLOB_BASE, 0, 0, 0) == 0);
			CHECK(rd(EVENT_SLOT) == EVENT_ORIG && rd(INPUT_SLOT) == INPUT_ORIG);
			CHECK(rd(BLOB_BASE + DS45_SHIM_OFF_DATA) == 0);

			pokes = fake_lv2_poke_count;
			kshim_remove();                       /* again: a harmless no-op */
			CHECK(fake_lv2_poke_count == pokes);
		}
	return 0;
}

/* Adoption restores every site the adopted install recorded as applied - the
 * output sites included - from the install's own originals, whatever the current
 * config says (like the inline words). A bit whose site is unpopulated in the row,
 * already changed, or not carrying our edit is skipped, never written; a row
 * without the sender address restores no output site; a version-7 block is not
 * adopted at all. */
static int test_output_adopt_v8(void)
{
	const uint64_t db = BLOB_BASE + DS45_SHIM_OFF_DATA;
	struct snap pristine, after;
	uint32_t mask;
	int pokes, g, mode, n;

	/* 1. adopt with another config and restore all of it */
	for (g = 0; g < 2; g++)
		for (mode = 0; mode < 3; mode++) {
			setup();
			plant_output_rows();
			g_cfg.shim_inline_pid = g;
			g_cfg.shim_output_hook = 1;
			g_cfg.shim_output_mode = mode;
			take_snap(&pristine);
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
			mask = 1u | (g ? 6u : 0u) | 8u | (mode ? 16u : 0u) | 96u;
			CHECK(pid_applied_at(BLOB_BASE) == mask);

			g_cfg.shim_inline_pid = !g;           /* the adopter's config differs */
			g_cfg.shim_output_hook = 0;
			g_cfg.shim_output_mode = 0;
			pokes = fake_lv2_poke_count;
			g_tr_applied_n = 0;
			CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
			CHECK(fake_lv2_poke_count == pokes);      /* zero pokes */
			CHECK(g_tr_applied_n == 1 && g_tr_applied == mask);
			CHECK(check_out_sites(BLOB_BASE, 1, mode != 0, 1) == 0);

			n = 1 + (g ? 2 : 0) + 1 + (mode != 0) + 2 + 3;
			kshim_unhook();
			CHECK(fake_lv2_poke_count - pokes == n);
			kshim_scrub();
			take_snap(&after);
			CHECK(snap_eq(&pristine, &after));
			CHECK(check_out_sites(BLOB_BASE, 0, 0, 0) == 0);
			CHECK(rd(EVENT_SLOT) == EVENT_ORIG && rd(INPUT_SLOT) == INPUT_ORIG);
			CHECK(fake_lv2_out_of_bounds == 0);
		}

	/* 2. a forged bit (site 4, never applied in observe mode: the cap qword is not
	 * carrying our word), a call site and a slot someone changed since: all three
	 * skipped and left as found; the intact slot 5, helper and redirects restored */
	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 0;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(pid_applied_at(BLOB_BASE) == (1u | 8u | 96u));
	put_q(db + offsetof(ds45_shim_data_t, pid_applied),
	      (uint64_t)(1u | 8u | 16u | 96u) << 32);
	put_q(db + offsetof(ds45_shim_data_t, pid_orig) + 8 * 4, OUT_Q(1));
	{
		const uint64_t call_changed = rd(OUT_CALL_QA) ^ 0x1;       /* the low half */
		const uint64_t slot6_changed = pktbuf_at(BLOB_BASE) + 0x20;

		put_q(OUT_CALL_QA, call_changed);
		put_q(OUT_SLOT6, slot6_changed);
		pokes = fake_lv2_poke_count;
		CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
		CHECK(fake_lv2_poke_count == pokes);
		kshim_unhook();
		CHECK(fake_lv2_poke_count - pokes == 1 + 1 + 3);   /* helper, slot 5, redirects */
		kshim_scrub();
		CHECK(rd(OUT_CALL_QA) == call_changed);               /* left as found */
		CHECK(rd(OUT_SLOT6) == slot6_changed);
		CHECK(rd(OUT_CAP_QA) == OUT_Q(1));                    /* never written */
		CHECK(rd(OUT_SLOT5) == OLD_BUF);                      /* restored */
		CHECK(rd(HELPER_QA) == HELPER_Q);
		CHECK(rd(EVENT_SLOT) == EVENT_ORIG && rd(INPUT_SLOT) == INPUT_ORIG);
	}

	/* 3. a site the adopter's row does not populate is not restored, even with its
	 * bit set and a good original; nor is any output site when the adopter's row
	 * has no sender address */
	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	memset(&g_ent.output_patches[3], 0, sizeof g_ent.output_patches[3]);
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_unhook();
	CHECK(fake_lv2_poke_count - pokes == 1 + 1 + 1 + 1 + 3);  /* all but slot 6 */
	kshim_scrub();
	CHECK(rd(OUT_SLOT6) == pktbuf_at(BLOB_BASE) + 1);         /* left as found */
	CHECK(rd(OUT_SLOT5) == OLD_BUF && rd(OUT_CALL_QA) == OUT_Q(0));
	CHECK(rd(OUT_CAP_QA) == OUT_Q(1));

	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	g_ent.orig_output = 0;
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_unhook();
	CHECK(fake_lv2_poke_count - pokes == 1 + 3);              /* helper + redirects */
	kshim_scrub();
	CHECK(check_out_sites(BLOB_BASE, 1, 1, 1) == 0);          /* all left as found */
	CHECK(fake_lv2_out_of_bounds == 0);

	/* 4. a version-7 block is not adopted (its layout is not ours) */
	setup();
	plant_output_rows();
	g_cfg.shim_output_hook = 1;
	g_cfg.shim_output_mode = 1;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_INSTALLED);
	CHECK(DS45_SHIM_VERSION == 9);
	put_q(db + offsetof(ds45_shim_data_t, version), ((uint64_t)7 << 32) | 1);
	g_spare_calls = 0;
	pokes = fake_lv2_poke_count;
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_FAILED);
	CHECK(fake_lv2_poke_count == pokes && g_spare_calls == 0);
	CHECK(check_out_sites(BLOB_BASE, 1, 1, 1) == 0);          /* left as found */
	put_q(db + offsetof(ds45_shim_data_t, version), ((uint64_t)DS45_SHIM_VERSION << 32) | 1);
	CHECK(kshim_install_from(&g_ent, 1) == KSHIM_ALREADY);
	kshim_remove();
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
	/* the connection table: the pointer array the handle indexes (TOC slot
	 * -0x3410), where each entry's +8..+0xF is the controller's address */
	CHECK(k->conn_tbl == 0x8000000000432690ULL);

	/* the template is the facts note's C1.5 array, identity bytes zero */
	CHECK(k->info_tmpl != NULL);
	CHECK(memcmp(k->info_tmpl, c15_template, DS45_SHIM_INFO_LEN) == 0);
	for (i = 8; i < 16; i++)
		CHECK(k->info_tmpl[i] == 0);

	/* acceptance: the helper always, as a branch at its first instruction to
	 * shim_pid_ok (its value is the blob address, filled in at install); the two
	 * inline words only with SHIM_INLINE_PID, which are WORD rows */
	CHECK(DS45_SHIM_PID_PATCHES == 1);
	CHECK(DS45_SHIM_INLINE_PATCHES == 2);
	CHECK(k->pid_patches[0].kind == DS45_KPATCH_BRANCH);
	CHECK(k->pid_patches[0].addr == 0x800000000029E2B4ULL);
	CHECK(k->pid_patches[0].expect == 0xF1B8B3EFF22A78FCULL);
	CHECK(k->pid_patches[0].value == 0);
	CHECK(k->inline_patches[0].kind == DS45_KPATCH_WORD);
	CHECK(k->inline_patches[0].addr == 0x800000000011B5E4ULL);
	CHECK(k->inline_patches[0].expect == 0xE8ADD0EDF3B2B3C1ULL);
	CHECK(k->inline_patches[0].value == 0x2F800CE6ULL);
	CHECK(k->inline_patches[1].kind == DS45_KPATCH_WORD);
	CHECK(k->inline_patches[1].addr == 0x800000000011B5ECULL);
	CHECK(k->inline_patches[1].expect == 0xF86140C9C3D7FB8DULL);
	CHECK(k->inline_patches[1].value == 0x4BFFFC48ULL);

	/* the DS3-route output rows (facts note 2026-11-03: F1.1 the two TOC slots,
	 * F5 the hashes): sites 3-6 in order, and the sender the hook tail-calls */
	CHECK(DS45_SHIM_OUTPUT_PATCHES == 4);
	CHECK(DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + DS45_SHIM_OUTPUT_PATCHES ==
	      DS45_SHIM_PID_SITES);
	CHECK(k->orig_output == 0x800000000010283CULL);
	/* [0] site 3: the bl to the sender, retargeted at install to the blob's entry */
	CHECK(k->output_patches[0].kind == DS45_KPATCH_BRANCHL);
	CHECK(k->output_patches[0].addr == 0x80000000002A2DE8ULL);
	CHECK(k->output_patches[0].expect == 0x2A29E6BB7BA10D5EULL);
	CHECK(k->output_patches[0].value == 0);
	/* [1] site 4: the send cap, cmplwi cr7,r31,0x4D (the compare is the low half) */
	CHECK(k->output_patches[1].kind == DS45_KPATCH_WORD);
	CHECK(k->output_patches[1].addr == 0x80000000000F8D74ULL);
	CHECK(k->output_patches[1].expect == 0xBF27C253F5228E13ULL);
	CHECK(k->output_patches[1].value == 0x2B9F004DULL);
	/* [2] [3] sites 5 and 6: the two TOC slots; a PTR row compares the current
	 * value (the old buffer, and the buffer plus one), the new one is the blob's
	 * packet buffer, filled in at install */
	CHECK(k->output_patches[2].kind == DS45_KPATCH_PTR);
	CHECK(k->output_patches[2].addr == 0x8000000000372650ULL);
	CHECK(k->output_patches[2].expect == 0x80000000004326C8ULL);
	CHECK(k->output_patches[2].value == 0);
	CHECK(k->output_patches[3].kind == DS45_KPATCH_PTR);
	CHECK(k->output_patches[3].addr == 0x8000000000372660ULL);
	CHECK(k->output_patches[3].expect == 0x80000000004326C9ULL);
	CHECK(k->output_patches[3].value == 0);
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
	/* the code sites the installer applies: the helper, the two inline words, the
	 * output call site and the cap word. The two output TOC slots (sites 5 and 6)
	 * are data, past the code image, and are planted below. */
	enum { NSITES = DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + 2,
	       CALL = DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES, CAP = CALL + 1 };
	uint64_t q[NSITES], qa[NSITES];
	long n;
	int inl, mode, i;

	for (inl = 0; inl < 2; inl++)
	for (mode = 0; mode < 2; mode++) {
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
		for (i = 0; i < NSITES; i++) {
			qa[i] = (i < DS45_SHIM_PID_PATCHES ? k->pid_patches[i].addr
			         : i < CALL ? k->inline_patches[i - DS45_SHIM_PID_PATCHES].addr
			         : k->output_patches[i - CALL].addr) & ~7ULL;
			CHECK(in_image(qa[i], n));
			q[i] = rd(qa[i]);
		}
		/* the output call site and the cap word are code (in the image) ... */
		CHECK(k->output_patches[0].addr == 0x80000000002A2DE8ULL);
		CHECK(k->output_patches[1].addr == 0x80000000000F8D74ULL);
		/* ... and the two TOC slots (0x372650, 0x372660) are past it, so they are
		 * planted with the values the row expects, like the other data slots */
		CHECK(!in_image(k->output_patches[2].addr, n) &&
		      !in_image(k->output_patches[3].addr, n));
		put_q(k->output_patches[2].addr, k->output_patches[2].expect);
		put_q(k->output_patches[3].addr, k->output_patches[3].expect);
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
		g_cfg.shim_output_hook = 1;
		g_cfg.shim_output_mode = mode;         /* 0 observe: no cap word; 1 probe */

		CHECK(kshim_install_from(ds45_shim_table, ds45_shim_table_count) ==
		      KSHIM_INSTALLED);
		CHECK(rd(k->event_slot) == k->exec_base + DS45_SHIM_OFF_EVENT);
		CHECK(rd(k->enable_slot) == k->exec_base + DS45_SHIM_OFF_ENABLE);
		CHECK(rd(k->input_slot) == k->exec_base + DS45_SHIM_OFF_INPUT);
		CHECK(rd(qa[0]) != q[0]);
		CHECK((rd(qa[1]) != q[1]) == (inl != 0));
		CHECK((rd(qa[2]) != q[2]) == (inl != 0));
		/* the call site is now a bl to the blob's output entry (high half of its
		 * qword, the low half kept), the cap word only outside observe mode, and
		 * the TOC slots hold the blob's packet buffer and that plus one */
		CHECK(rd(qa[CALL]) == (((uint64_t)ppc_bl(k->output_patches[0].addr,
		      k->exec_base + DS45_SHIM_OFF_OUTPUT) << 32) | (q[CALL] & 0xFFFFFFFFULL)));
		CHECK((rd(qa[CAP]) != q[CAP]) == (mode != 0));
		if (mode)
			CHECK(rd(qa[CAP]) == ((q[CAP] & HI32) | 0x2B9F004DULL));
		CHECK(rd(k->output_patches[2].addr) == pktbuf_at(k->exec_base));
		CHECK(rd(k->output_patches[3].addr) == pktbuf_at(k->exec_base) + 1);

		kshim_remove();
		for (i = 0; i < NSITES; i++)
			CHECK(rd(qa[i]) == q[i]);
		CHECK(rd(k->output_patches[2].addr) == k->output_patches[2].expect);
		CHECK(rd(k->output_patches[3].addr) == k->output_patches[3].expect);
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
		test_stable_id_switch,
		test_output_fields,
		test_output_sites_hook_off,
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
		test_helper_branch_row,
		test_output_sites_gated,
		test_output_site_index_mapping,
		test_output_branchl_follows_load_base,
		test_buffer_slots_point_into_blob,
		test_output_site_mismatch_fails_clean,
		test_output_branchl_out_of_range_refused,
		test_output_rollback_each_step,
		test_output_apply_order_and_block_complete,
		test_output_remove_restores_all,
		test_output_adopt_v8,
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
