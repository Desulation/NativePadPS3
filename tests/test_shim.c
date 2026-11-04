/* SOURCES: ds_body.c sixaxis.c ../shim/ds3_report.c ../shim/ds3_output.c ../shim/shim.c ../shim/entry.c */
/*
 * Host tests for the kernel shim (data block v3): the decision logic in
 * shim/shim.c - the product-ID helper, the start-up and enable completions,
 * the pad identity, the pad-manager lookup and the input filter - and the
 * kernel-ABI glue in shim/entry.c that calls it. The decision functions take the
 * data block and plain byte buffers standing in for the kernel's driver, pad and
 * pad-manager objects, so they are pure and testable with no kernel calls.
 * entry.c is linked in as well: its wrappers call the originals through a
 * descriptor address, which a host function can fake, so which events complete
 * and which tail-call the original are exercised here too.
 *
 * The object layouts are the DEX 4.92 ones (shim.h, facts note C1-C3); every
 * object here is synthetic.
 */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "ds45_state.h"
#include "../shim/shim_abi.h"
#include "../shim/ds3_report.h"
#include "../shim/ds3_output.h"
#include "../shim/shim.h"

/* shim/entry.c: the one data object the plugin and the blob share, and the
 * three wrappers the kernel's redirects point at. */
extern ds45_shim_data_t ds45_shim_data;
int shim_event(void *drv, long status, long len);
int shim_enable(void *drv, long status, long len);
int shim_input(void *xport, uint8_t *dst, uint16_t len_in, int flag);
int shim_output(const uint8_t *buf, uint16_t len, uint8_t *rid,
                const uint8_t **obuf, uint16_t *olen);

/* Sony USB identity. */
#define VID_SONY        0x054C
#define PID_DS3         0x0268
#define PID_MOVE_NAV    0x042F
#define PID_DS4         0x05C4
#define PID_DUALSENSE   0x0CE6

#define PAD_VTABLE      0x800000000038CDC8ULL   /* the Sony pad class (DEX 4.92) */

#define RPT_LEN   78                   /* a whole Bluetooth report, CRC included */
#define BT(f)     (DS_OFF_BT + (f))    /* index of common-block field f in it    */
#define BTN(n)    BT(DS_C_BUTTONS + (n))

/* A neutral DualSense Bluetooth 0x31 report: sticks centred, D-pad released,
 * everything else zero (as tests/test_ds3report.c's report_neutral()). */
static void report_neutral(uint8_t r[RPT_LEN])
{
	memset(r, 0, RPT_LEN);
	r[0] = 0x31;
	r[1] = 0x01;
	r[BT(DS_C_LX)] = r[BT(DS_C_LY)] = 0x80;
	r[BT(DS_C_RX)] = r[BT(DS_C_RY)] = 0x80;
	r[BTN(0)] = 0x08;              /* hat 8 = released */
}

/* A DualSense basic Bluetooth report (ds3_report.h): id 0x01, sticks, hat in the
 * low nibble of [5], two more button bytes, L2/R2 analog. Cross and R1 held and
 * the left stick off-centre, so the translation is not the neutral one. */
static void report_basic(uint8_t r[DS_BASIC_LEN])
{
	memset(r, 0, DS_BASIC_LEN);
	r[0] = DS_BASIC_REPORT_ID;
	r[1] = 0x10;
	r[2] = 0xF0;
	r[3] = r[4] = 0x80;
	r[5] = 0x08 | 0x20;            /* hat released, Cross */
	r[6] = 0x02;                   /* R1 */
	r[9] = 0x40;                   /* R2 analog */
}

/* The DS3 report a neutral DualSense translates to with motion on and no
 * motion: the report id, the four centred sticks, and the four 512 sensor
 * words (as tests/test_ds3report.c's ds3_neutral()). */
static void ds3_neutral(uint8_t e[DS3_REPORT_LEN])
{
	int i;

	memset(e, 0, DS3_REPORT_LEN);
	e[0] = 0x01;
	for (i = 6; i <= 9; i++)
		e[i] = 0x80;
	e[30] = 0x01;                  /* battery: status 0 is level 1 */
	for (i = 0; i < 4; i++)
		e[41 + 2 * i] = 0x02;      /* 512 = 0x0200, big-endian */
}

static ds45_shim_cfg_t cfg_plain(void)
{
	ds45_shim_cfg_t c;

	memset(&c, 0, sizeof(c));
	c.sixaxis = 1;
	c.accel_scale = 100;
	c.gyro_scale = 100;
	return c;
}

/* The template the tests install: a pattern, so a copy is visible byte for byte,
 * with the identity bytes (index 0x08..0x0F) zero as in the real template. */
static void tmpl_fill(uint8_t t[0x58])
{
	int i;

	memset(t, 0, 0x58);
	for (i = 0; i < DS45_SHIM_INFO_LEN; i++)
		t[i] = (uint8_t)(i * 5 + 3);
	for (i = SHIM_ID_OFF; i < SHIM_ID_OFF + 8; i++)
		t[i] = 0;
}

/* A zeroed, enabled data block with a plain config, the pad vtable and the
 * template (magic/version pre-filled, as the blob ships them). */
static ds45_shim_data_t data_enabled(void)
{
	ds45_shim_data_t d;

	memset(&d, 0, sizeof(d));
	d.magic = DS45_SHIM_MAGIC;
	d.version = DS45_SHIM_VERSION;
	d.enabled = 1;
	d.pad_vtable = PAD_VTABLE;
	d.cfg = cfg_plain();
	tmpl_fill(d.info_tmpl);
	return d;
}

/* ------------------------------------------------------------------------ */
/* Fake kernel objects. Unions keep the native u64 fields 8-aligned.          */
/* ------------------------------------------------------------------------ */

#define PAD_LEN   0x520
#define DRV_LEN   0xB0
#define EN_LEN    0x98
#define MGR_LEN   (SHIM_MGR_OBJS + SHIM_MGR_NOBJS * 8)

typedef union { uint64_t q[PAD_LEN / 8]; uint8_t b[PAD_LEN]; } fake_pad_t;
typedef union { uint64_t q[DRV_LEN / 8]; uint8_t b[DRV_LEN]; } fake_drv_t;
typedef union { uint64_t q[EN_LEN / 8];  uint8_t b[EN_LEN];  } fake_en_t;
typedef union { uint64_t q[MGR_LEN / 8]; uint8_t b[MGR_LEN]; } fake_mgr_t;
typedef union { uint64_t q[2];           uint8_t b[16];      } fake_xport_t;

static void put_native(uint8_t *at, const void *p)
{
	*(uint64_t *)(void *)at = (uint64_t)(uintptr_t)p;
}

/* A live pad object of class `vtable` whose +0x68 reads VID/PID big-endian, with
 * garbage everywhere else (so any stray write shows). */
static void pad_make(fake_pad_t *p, uint64_t vtable, uint16_t vid, uint16_t pid)
{
	memset(p->b, 0x5A, sizeof(p->b));
	p->q[0] = vtable;
	p->b[SHIM_PAD_ID + 0] = (uint8_t)(vid >> 8);
	p->b[SHIM_PAD_ID + 1] = (uint8_t)vid;
	p->b[SHIM_PAD_ID + 2] = (uint8_t)(pid >> 8);
	p->b[SHIM_PAD_ID + 3] = (uint8_t)pid;
}

/* A start-up driver mid-retry: its pad, the info pointer pad+0x80, a refusal
 * status 0x8001000C and substate 3. */
static void drv_make(fake_drv_t *d, fake_pad_t *pad)
{
	memset(d->b, 0x5A, sizeof(d->b));
	put_native(d->b + SHIM_DRV_PAD, pad ? pad->b : NULL);
	put_native(d->b + SHIM_DRV_INFO, pad ? pad->b + 0x80 : NULL);
	d->b[SHIM_DRV_STATUS + 0] = 0x80;
	d->b[SHIM_DRV_STATUS + 1] = 0x01;
	d->b[SHIM_DRV_STATUS + 2] = 0x00;
	d->b[SHIM_DRV_STATUS + 3] = 0x0C;
	d->b[SHIM_DRV_SUBSTATE] = 3;
}

/* An enable (0xF4) driver: its pad, a refusal status, not done. */
static void en_make(fake_en_t *e, fake_pad_t *pad)
{
	memset(e->b, 0x5A, sizeof(e->b));
	put_native(e->b + SHIM_DRV_PAD, pad ? pad->b : NULL);
	e->b[SHIM_DRV_STATUS + 0] = 0x80;
	e->b[SHIM_DRV_STATUS + 1] = 0x01;
	e->b[SHIM_DRV_STATUS + 2] = 0x00;
	e->b[SHIM_DRV_STATUS + 3] = 0x0C;
	e->b[SHIM_EN_DONE] = 0;
}

/* ------------------------------------------------------------------------ */
/* Product-ID acceptance (kept for stage 1b)                                  */
/* ------------------------------------------------------------------------ */

static int test_pid_ok_table(void)
{
	/* accepted: Sony VID with the DS3, Move-navigation or DualSense PID */
	CHECK(shim_pid_ok(VID_SONY, PID_DS3) == 1);
	CHECK(shim_pid_ok(VID_SONY, PID_MOVE_NAV) == 1);
	CHECK(shim_pid_ok(VID_SONY, PID_DUALSENSE) == 1);

	/* rejected: a DS4, the wrong VID, a Move motion controller (0x03D5), zeros */
	CHECK(shim_pid_ok(VID_SONY, PID_DS4) == 0);
	CHECK(shim_pid_ok(0x054D, PID_DUALSENSE) == 0);
	CHECK(shim_pid_ok(VID_SONY, 0x03D5) == 0);
	CHECK(shim_pid_ok(VID_SONY, 0x0000) == 0);
	CHECK(shim_pid_ok(0x0000, PID_DS3) == 0);
	CHECK(shim_pid_ok(0x046D, 0xC260) == 0);

	/* shim_is_dualsense is the narrower gate the input path keys on */
	CHECK(shim_is_dualsense(VID_SONY, PID_DUALSENSE) == 1);
	CHECK(shim_is_dualsense(VID_SONY, PID_DS3) == 0);
	CHECK(shim_is_dualsense(VID_SONY, PID_DS4) == 0);
	CHECK(shim_is_dualsense(0x054D, PID_DUALSENSE) == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Pad identity                                                               */
/* ------------------------------------------------------------------------ */

static int test_pad_identity(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;

	CHECK(shim_pad_is_dualsense(&d, NULL) == 0);

	pad_make(&pad, PAD_VTABLE + 0x40, VID_SONY, PID_DUALSENSE);   /* another class */
	CHECK(shim_pad_is_dualsense(&d, pad.b) == 0);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	CHECK(shim_pad_is_dualsense(&d, pad.b) == 0);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	CHECK(shim_pad_is_dualsense(&d, pad.b) == 1);

	/* byte order: the PID's bytes swapped is not a DualSense */
	pad.b[SHIM_PAD_ID + 2] = 0xE6;
	pad.b[SHIM_PAD_ID + 3] = 0x0C;
	CHECK(shim_pad_is_dualsense(&d, pad.b) == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Start-up event completion                                                  */
/* ------------------------------------------------------------------------ */

static int test_event_dualsense_completes(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad, pad_before;
	fake_drv_t drv, drv_before;
	static const uint8_t id1[8] = { 0x00, 0x00, 0x00, 0x01, 0xD5, 0x00, 0x44, 0x53 };
	int i;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	pad_before = pad;
	drv_before = drv;

	CHECK(shim_event_complete(&d, drv.b) == 1);

	/* the template outside the identity, the identity from the counter */
	CHECK(memcmp(pad.b + 0x80, d.info_tmpl, 8) == 0);
	CHECK(memcmp(pad.b + 0x90, d.info_tmpl + 0x10, DS45_SHIM_INFO_LEN - 0x10) == 0);
	CHECK(memcmp(pad.b + 0x88, id1, 8) == 0);
	/* nothing of the pad outside +0x80..+0xD5 */
	CHECK(memcmp(pad.b, pad_before.b, 0x80) == 0);
	CHECK(memcmp(pad.b + 0x80 + DS45_SHIM_INFO_LEN, pad_before.b + 0x80 + DS45_SHIM_INFO_LEN,
	             PAD_LEN - 0x80 - DS45_SHIM_INFO_LEN) == 0);

	/* status 0 (4 bytes), substate 9 - and no other driver byte */
	for (i = 0; i < 4; i++)
		CHECK(drv.b[SHIM_DRV_STATUS + i] == 0);
	CHECK(drv.b[SHIM_DRV_SUBSTATE] == SHIM_DRV_DONE);
	CHECK(SHIM_DRV_DONE == 9);
	for (i = 0; i < DRV_LEN; i++)
		if (i < SHIM_DRV_STATUS || i > SHIM_DRV_SUBSTATE)
			CHECK(drv.b[i] == drv_before.b[i]);

	CHECK(d.stat_completed == 1);
	CHECK(d.stat_enabled == 0);
	return 0;
}

static int test_event_identities_differ(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pa, pb;
	fake_drv_t da, db;

	pad_make(&pa, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	pad_make(&pb, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&da, &pa);
	drv_make(&db, &pb);

	CHECK(shim_event_complete(&d, da.b) == 1);
	CHECK(shim_event_complete(&d, db.b) == 1);

	CHECK(pa.b[0x88] == 0 && pa.b[0x89] == 0 && pa.b[0x8A] == 0 && pa.b[0x8B] == 1);
	CHECK(pb.b[0x88] == 0 && pb.b[0x89] == 0 && pb.b[0x8A] == 0 && pb.b[0x8B] == 2);
	CHECK(pb.b[0x8C] == 0xD5 && pb.b[0x8D] == 0 && pb.b[0x8E] == 0x44 && pb.b[0x8F] == 0x53);
	CHECK(memcmp(pa.b + 0x88, pb.b + 0x88, 8) != 0);
	CHECK(d.stat_completed == 2);

	/* the counter is written big-endian over all four bytes */
	d.stat_completed = 0x01020304 - 1;
	pad_make(&pa, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&da, &pa);
	CHECK(shim_event_complete(&d, da.b) == 1);
	CHECK(pa.b[0x88] == 0x01 && pa.b[0x89] == 0x02 && pa.b[0x8A] == 0x03 && pa.b[0x8B] == 0x04);
	return 0;
}

/* Run shim_event_complete on a driver/pad pair that must pass to the original:
 * it returns 0 and changes neither object nor the data block. */
static int event_passes(ds45_shim_data_t *d, fake_drv_t *drv, fake_pad_t *pad)
{
	fake_drv_t drv_before = *drv;
	fake_pad_t pad_before = *pad;
	ds45_shim_data_t d_before;

	memcpy(&d_before, d, sizeof(d_before));
	CHECK(shim_event_complete(d, drv->b) == 0);
	CHECK(memcmp(drv->b, drv_before.b, DRV_LEN) == 0);
	CHECK(memcmp(pad->b, pad_before.b, PAD_LEN) == 0);
	/* Pass-through writes nothing lasting, but the event hook bumps its debug
	 * counters on every entry (and dbg_event_ds whenever the pad is a DualSense),
	 * so normalise those before comparing the rest of the data block. */
	d_before.dbg_event_calls = d->dbg_event_calls;
	d_before.dbg_event_ds = d->dbg_event_ds;
	CHECK(memcmp(d, &d_before, sizeof(*d)) == 0);
	return 0;
}

static int test_event_after_done_passes(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_drv_t drv;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	drv.b[SHIM_DRV_SUBSTATE] = SHIM_DRV_DONE;
	CHECK(event_passes(&d, &drv, &pad) == 0);
	return 0;
}

static int test_event_ds3_untouched(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_drv_t drv;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	drv_make(&drv, &pad);
	CHECK(event_passes(&d, &drv, &pad) == 0);
	return 0;
}

static int test_event_bad_pad_untouched(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_drv_t drv;

	/* no pad object */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, NULL);
	CHECK(event_passes(&d, &drv, &pad) == 0);

	/* a pad of another class */
	pad_make(&pad, PAD_VTABLE ^ 0x1000, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	CHECK(event_passes(&d, &drv, &pad) == 0);

	/* and a NULL driver never crashes */
	CHECK(shim_event_complete(&d, NULL) == 0);
	CHECK(d.stat_completed == 0);
	return 0;
}

static int test_event_disabled(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_drv_t drv;

	d.enabled = 0;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	CHECK(event_passes(&d, &drv, &pad) == 0);
	return 0;
}

/* The info pointer at drv+0x58 is only trusted when it is the validated pad's
 * own +0x80: pointing anywhere else (another buffer, the pad at another offset,
 * NULL) the event passes to the original and not a byte of the driver, the pad,
 * the other buffer or the data block changes. */
static int test_event_info_not_pad_untouched(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad, other, other_before;
	fake_drv_t drv;
	static const int pad_offs[] = { 0, 0x78, 0x88 };
	unsigned i;

	pad_make(&other, 0, 0, 0);
	other_before = other;

	/* another object's +0x80 */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	put_native(drv.b + SHIM_DRV_INFO, other.b + 0x80);
	CHECK(event_passes(&d, &drv, &pad) == 0);
	CHECK(memcmp(other.b, other_before.b, PAD_LEN) == 0);

	/* the right pad at the wrong offset */
	for (i = 0; i < sizeof(pad_offs) / sizeof(pad_offs[0]); i++) {
		pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
		drv_make(&drv, &pad);
		put_native(drv.b + SHIM_DRV_INFO, pad.b + pad_offs[i]);
		CHECK(event_passes(&d, &drv, &pad) == 0);
	}

	/* no info block at all */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	put_native(drv.b + SHIM_DRV_INFO, NULL);
	CHECK(event_passes(&d, &drv, &pad) == 0);
	CHECK(d.stat_completed == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Enable (0xF4) event completion                                             */
/* ------------------------------------------------------------------------ */

static int test_enable_dualsense_completes(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad, pad_before;
	fake_en_t en, en_before;
	int i;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	en_make(&en, &pad);
	pad_before = pad;
	en_before = en;

	CHECK(shim_enable_complete(&d, en.b) == 1);

	for (i = 0; i < 4; i++)
		CHECK(en.b[SHIM_DRV_STATUS + i] == 0);
	CHECK(en.b[SHIM_EN_DONE] == 1);
	for (i = 0; i < EN_LEN; i++)
		if ((i < SHIM_DRV_STATUS || i > SHIM_DRV_STATUS + 3) && i != SHIM_EN_DONE)
			CHECK(en.b[i] == en_before.b[i]);
	CHECK(memcmp(pad.b, pad_before.b, PAD_LEN) == 0);     /* the pad is not touched */
	CHECK(d.stat_enabled == 1);
	CHECK(d.stat_completed == 0);
	return 0;
}

static int enable_passes(ds45_shim_data_t *d, fake_en_t *en, fake_pad_t *pad)
{
	fake_en_t en_before = *en;
	fake_pad_t pad_before = *pad;
	ds45_shim_data_t d_before;

	memcpy(&d_before, d, sizeof(d_before));
	CHECK(shim_enable_complete(d, en->b) == 0);
	CHECK(memcmp(en->b, en_before.b, EN_LEN) == 0);
	CHECK(memcmp(pad->b, pad_before.b, PAD_LEN) == 0);
	/* As event_passes: the enable hook bumps dbg_enable_calls on every entry and
	 * dbg_enable_ds whenever the pad is a DualSense, even on pass-through. */
	d_before.dbg_enable_calls = d->dbg_enable_calls;
	d_before.dbg_enable_ds = d->dbg_enable_ds;
	CHECK(memcmp(d, &d_before, sizeof(*d)) == 0);
	return 0;
}

static int test_enable_passes(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_en_t en;

	/* a DS3 */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	en_make(&en, &pad);
	CHECK(enable_passes(&d, &en, &pad) == 0);

	/* no pad */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	en_make(&en, NULL);
	CHECK(enable_passes(&d, &en, &pad) == 0);

	/* another class */
	pad_make(&pad, PAD_VTABLE + 8, VID_SONY, PID_DUALSENSE);
	en_make(&en, &pad);
	CHECK(enable_passes(&d, &en, &pad) == 0);

	/* already done */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	en_make(&en, &pad);
	en.b[SHIM_EN_DONE] = 1;
	CHECK(enable_passes(&d, &en, &pad) == 0);

	/* disabled */
	en_make(&en, &pad);
	d.enabled = 0;
	CHECK(enable_passes(&d, &en, &pad) == 0);

	CHECK(shim_enable_complete(&d, NULL) == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Pad-manager lookup                                                         */
/* ------------------------------------------------------------------------ */

/* The fake pad manager: a DS3 in entry 0, nulls, the DualSense in entry 5. Each
 * pad's +0x4F0 points at its own transport object. */
static fake_mgr_t   g_mgr;
static uint64_t     g_mgr_ptr;          /* the global holding the manager pointer */
static fake_pad_t   g_pad_ds, g_pad_ds3;
static fake_xport_t g_x_ds, g_x_ds3, g_x_other;

static void mgr_make(ds45_shim_data_t *d)
{
	memset(g_mgr.b, 0, sizeof(g_mgr.b));
	memset(g_mgr.b, 0x77, SHIM_MGR_OBJS);           /* the manager's other fields */
	pad_make(&g_pad_ds3, PAD_VTABLE, VID_SONY, PID_DS3);
	pad_make(&g_pad_ds, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	memset(&g_x_ds, 0, sizeof(g_x_ds));
	memset(&g_x_ds3, 0, sizeof(g_x_ds3));
	memset(&g_x_other, 0, sizeof(g_x_other));
	put_native(g_pad_ds3.b + SHIM_PAD_XPORT, &g_x_ds3);
	put_native(g_pad_ds.b + SHIM_PAD_XPORT, &g_x_ds);
	put_native(g_mgr.b + SHIM_MGR_OBJS + 0 * 8, g_pad_ds3.b);
	put_native(g_mgr.b + SHIM_MGR_OBJS + 5 * 8, g_pad_ds.b);
	g_mgr_ptr = (uint64_t)(uintptr_t)g_mgr.b;
	d->pad_mgr = (uint64_t)(uintptr_t)&g_mgr_ptr;
}

static int test_pad_from_xport(void)
{
	ds45_shim_data_t d = data_enabled();

	mgr_make(&d);
	CHECK(shim_pad_from_xport(&d, &g_x_ds) == g_pad_ds.b);
	CHECK(shim_pad_from_xport(&d, &g_x_ds3) == g_pad_ds3.b);
	CHECK(shim_pad_from_xport(&d, &g_x_other) == NULL);
	CHECK(shim_pad_from_xport(&d, NULL) == NULL);

	/* the last entry is scanned, and nothing past it */
	put_native(g_mgr.b + SHIM_MGR_OBJS + 5 * 8, NULL);
	put_native(g_mgr.b + SHIM_MGR_OBJS + 15 * 8, g_pad_ds.b);
	CHECK(SHIM_MGR_NOBJS == 16);
	CHECK(shim_pad_from_xport(&d, &g_x_ds) == g_pad_ds.b);

	/* no manager global, or a manager not yet created */
	d.pad_mgr = 0;
	CHECK(shim_pad_from_xport(&d, &g_x_ds) == NULL);
	mgr_make(&d);
	g_mgr_ptr = 0;
	CHECK(shim_pad_from_xport(&d, &g_x_ds) == NULL);
	CHECK(shim_pad_from_xport(NULL, &g_x_ds) == NULL);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Input filter                                                               */
/* ------------------------------------------------------------------------ */

/* A DualShock 3's own report passes straight through: not a DualSense. */
static int test_input_ds3_untouched(void)
{
	ds45_shim_data_t d = data_enabled();
	uint8_t buf[64], orig[64];
	uint16_t n = DS3_REPORT_LEN;
	int i, rc;

	for (i = 0; i < 64; i++)
		buf[i] = orig[i] = (uint8_t)(i * 7 + 1);
	buf[0] = orig[0] = 0x01;       /* a DualShock 3 input report */

	rc = shim_input_filter(&d, VID_SONY, PID_DS3, buf, &n);

	CHECK(rc == DS3_REPORT_LEN);           /* original result returned */
	CHECK(n == DS3_REPORT_LEN);            /* length unchanged */
	CHECK(memcmp(buf, orig, 64) == 0);     /* byte-identical */
	CHECK(d.stat_passed == 1);
	CHECK(d.stat_translated == 0);
	return 0;
}

/* A DualSense 0x31 report is rewritten in place to a 49-byte DS3 report. */
static int test_input_dualsense_translated(void)
{
	ds45_shim_data_t d = data_enabled();
	uint8_t report[RPT_LEN], buf[RPT_LEN], want[DS3_REPORT_LEN];
	uint16_t n = 64;               /* the driver buffer delivers at most 64 bytes */
	int rc;

	report_neutral(report);
	memcpy(buf, report, RPT_LEN);
	ds3_neutral(want);

	rc = shim_input_filter(&d, VID_SONY, PID_DUALSENSE, buf, &n);

	CHECK(rc == DS3_REPORT_LEN);           /* 49 */
	CHECK(n == DS3_REPORT_LEN);
	CHECK(buf[0] == 0x01);
	CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0);
	CHECK(d.stat_translated == 1);
	CHECK(d.stat_passed == 0);
	return 0;
}

/* A report too short for the translator passes through untouched. */
static int test_input_short_passthrough(void)
{
	ds45_shim_data_t d = data_enabled();
	uint8_t report[RPT_LEN], buf[RPT_LEN];
	uint16_t n = DS_BT_BODY_MIN - 1;       /* one byte too short to translate */
	int rc;

	report_neutral(report);
	memcpy(buf, report, RPT_LEN);

	rc = shim_input_filter(&d, VID_SONY, PID_DUALSENSE, buf, &n);

	CHECK(rc == (int)(DS_BT_BODY_MIN - 1));
	CHECK(n == DS_BT_BODY_MIN - 1);
	CHECK(memcmp(buf, report, RPT_LEN) == 0);
	CHECK(d.stat_passed == 1);
	CHECK(d.stat_translated == 0);
	return 0;
}

/* enabled == 0 makes the filter behave exactly like the original. */
static int test_input_disabled_passthrough(void)
{
	ds45_shim_data_t d = data_enabled();
	uint8_t report[RPT_LEN], buf[RPT_LEN];
	uint16_t n = 64;
	int rc;

	d.enabled = 0;
	report_neutral(report);
	memcpy(buf, report, RPT_LEN);

	rc = shim_input_filter(&d, VID_SONY, PID_DUALSENSE, buf, &n);

	CHECK(rc == 64);
	CHECK(n == 64);
	CHECK(memcmp(buf, report, RPT_LEN) == 0);
	CHECK(d.stat_passed == 1);
	CHECK(d.stat_translated == 0);
	return 0;
}

/* An unknown identity (entry.c passes 0/0 when the pad is not a DualSense)
 * leaves the buffer untouched and returns the original result. */
static int test_input_identity_unknown_passthrough(void)
{
	ds45_shim_data_t d = data_enabled();
	uint8_t report[RPT_LEN], buf[RPT_LEN];
	uint16_t n = 64;
	int rc;

	report_neutral(report);        /* a real 0x31, but the identity is unknown */
	memcpy(buf, report, RPT_LEN);

	rc = shim_input_filter(&d, 0x0000, 0x0000, buf, &n);

	CHECK(rc == 64);               /* original result returned */
	CHECK(n == 64);
	CHECK(memcmp(buf, report, RPT_LEN) == 0);
	CHECK(d.stat_passed == 1);
	CHECK(d.stat_translated == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Glue (entry.c)                                                             */
/* ------------------------------------------------------------------------ */

#define ORIG_EVENT_RC   0x1357
#define ORIG_ENABLE_RC  0x2468

static int      g_oe_calls, g_on_calls, g_oi_calls;
static void    *g_oe_drv, *g_on_drv;
static long     g_oe_status, g_oe_len, g_on_status, g_on_len;
static uint8_t  g_oi_report[96];       /* what the original read delivers ... */
static int      g_oi_len;              /* ... and how many bytes it reports (< 0: an error, nothing copied) */
static void    *g_oi_xport;            /* what the last call to the original was given */
static uint8_t *g_oi_dst;
static uint16_t g_oi_len_in;
static int      g_oi_flag;

static int fake_orig_event(void *drv, long status, long len)
{
	g_oe_calls++;
	g_oe_drv = drv;
	g_oe_status = status;
	g_oe_len = len;
	return ORIG_EVENT_RC;
}

static int fake_orig_enable(void *drv, long status, long len)
{
	g_on_calls++;
	g_on_drv = drv;
	g_on_status = status;
	g_on_len = len;
	return ORIG_ENABLE_RC;
}

static int fake_orig_input(void *xport, uint8_t *dst, uint16_t len_in, int flag)
{
	g_oi_calls++;
	g_oi_xport = xport;
	g_oi_dst = dst;
	g_oi_len_in = len_in;
	g_oi_flag = flag;
	/* Like the transport read: copy the whole packet into dst with no bound. */
	if (dst && g_oi_len > 0)
		memcpy(dst, g_oi_report, (size_t)g_oi_len);
	return g_oi_len;
}

/* The glue works on the real, global data block: reset it to enabled, with the
 * fakes above as the originals and the fake pad manager. */
static void glue_reset(void)
{
	ds45_shim_data = data_enabled();
	ds45_shim_data.orig_event  = (uint64_t)(uintptr_t)fake_orig_event;
	ds45_shim_data.orig_enable = (uint64_t)(uintptr_t)fake_orig_enable;
	ds45_shim_data.orig_input  = (uint64_t)(uintptr_t)fake_orig_input;
	mgr_make(&ds45_shim_data);
	g_oe_calls = g_on_calls = g_oi_calls = 0;
	g_oe_drv = g_on_drv = NULL;
	g_oe_status = g_oe_len = g_on_status = g_on_len = -1;
	memset(g_oi_report, 0, sizeof(g_oi_report));
	g_oi_len = 0;
	g_oi_xport = (void *)1;
	g_oi_dst = (uint8_t *)1;
	g_oi_len_in = 0xFFFF;
	g_oi_flag = -1;
}

/* A DualSense's first start-up event is answered 0 without the original; the
 * next one (substate 9) and a DS3's go to the original with the same arguments
 * and its result is returned. */
static int test_glue_event(void)
{
	fake_drv_t drv;

	glue_reset();
	drv_make(&drv, &g_pad_ds);
	CHECK(shim_event(drv.b, (long)0x8001000CL, 4) == 0);
	CHECK(g_oe_calls == 0);
	CHECK(drv.b[SHIM_DRV_SUBSTATE] == SHIM_DRV_DONE);
	CHECK(ds45_shim_data.stat_completed == 1);

	CHECK(shim_event(drv.b, 0, 4) == ORIG_EVENT_RC);       /* already done */
	CHECK(g_oe_calls == 1 && g_oe_drv == (void *)drv.b);
	CHECK(g_oe_status == 0 && g_oe_len == 4);

	drv_make(&drv, &g_pad_ds3);
	CHECK(shim_event(drv.b, 7, 9) == ORIG_EVENT_RC);
	CHECK(g_oe_calls == 2 && g_oe_status == 7 && g_oe_len == 9);
	CHECK(ds45_shim_data.stat_completed == 1);

	CHECK(shim_event(NULL, 1, 2) == ORIG_EVENT_RC);
	CHECK(g_oe_calls == 3 && g_oe_drv == NULL);
	CHECK(g_on_calls == 0);                                /* never the other original */
	return 0;
}

static int test_glue_enable(void)
{
	fake_en_t en;

	glue_reset();
	en_make(&en, &g_pad_ds);
	CHECK(shim_enable(en.b, (long)0x8001000CL, 4) == 0);
	CHECK(g_on_calls == 0);
	CHECK(en.b[SHIM_EN_DONE] == 1);
	CHECK(ds45_shim_data.stat_enabled == 1);

	CHECK(shim_enable(en.b, 0, 4) == ORIG_ENABLE_RC);      /* already done */
	CHECK(g_on_calls == 1 && g_on_drv == (void *)en.b);
	CHECK(g_on_status == 0 && g_on_len == 4);

	en_make(&en, &g_pad_ds3);
	CHECK(shim_enable(en.b, 5, 6) == ORIG_ENABLE_RC);
	CHECK(g_on_calls == 2 && g_on_status == 5 && g_on_len == 6);

	ds45_shim_data.enabled = 0;
	en_make(&en, &g_pad_ds);
	CHECK(shim_enable(en.b, 5, 6) == ORIG_ENABLE_RC);
	CHECK(g_on_calls == 3 && en.b[SHIM_EN_DONE] == 0);
	CHECK(g_oe_calls == 0);
	return 0;
}

/* The DualSense, found through the pad manager by its transport, has its basic
 * report rewritten in place to the 49-byte DS3 report. */
static int test_input_basic_translated(void)
{
	uint8_t dst[64], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t cfg;

	glue_reset();
	cfg = ds45_shim_data.cfg;
	report_basic(g_oi_report);
	g_oi_len = DS_BASIC_LEN;
	CHECK(ds3_report_from_dualsense_basic(g_oi_report, DS_BASIC_LEN, &cfg, want) == 0);
	memset(dst, 0xCC, sizeof(dst));

	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == DS3_REPORT_LEN);
	CHECK(g_oi_calls == 1);
	CHECK(dst[0] == 0x01);
	CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);
	CHECK(ds45_shim_data.stat_translated == 1);
	CHECK(ds45_shim_data.stat_passed == 0);

	/* the 0x31 path is still there for stage 2 */
	report_neutral(g_oi_report);
	g_oi_len = 64;
	ds3_neutral(want);
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == DS3_REPORT_LEN);
	CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);
	CHECK(ds45_shim_data.stat_translated == 2);
	return 0;
}

/* Everything that is not a DualSense's basic (or 0x31) report comes back as the
 * original read left it, and each such read is counted as passed. */
static int test_input_passthrough(void)
{
	uint8_t dst[64], want[64];
	int i;

	glue_reset();

	/* 1. a DS3, found in the manager: its own 0x01 report */
	for (i = 0; i < RPT_LEN; i++)
		g_oi_report[i] = (uint8_t)(i * 3 + 1);
	g_oi_report[0] = 0x01;
	g_oi_len = DS3_REPORT_LEN;
	memset(dst, 0xCC, sizeof(dst));
	memset(want, 0xCC, sizeof(want));
	memcpy(want, g_oi_report, (size_t)g_oi_len);
	CHECK(shim_input(&g_x_ds3, dst, sizeof(dst), 0) == DS3_REPORT_LEN);
	CHECK(memcmp(dst, want, sizeof(dst)) == 0);
	CHECK(ds45_shim_data.stat_passed == 1);

	/* 2. a transport no pad in the manager owns, with a basic report */
	report_basic(g_oi_report);
	g_oi_len = DS_BASIC_LEN;
	memset(dst, 0xCC, sizeof(dst));
	memset(want, 0xCC, sizeof(want));
	memcpy(want, g_oi_report, (size_t)g_oi_len);
	CHECK(shim_input(&g_x_other, dst, sizeof(dst), 0) == DS_BASIC_LEN);
	CHECK(memcmp(dst, want, sizeof(dst)) == 0);
	CHECK(ds45_shim_data.stat_passed == 2);

	/* 3. the DualSense, but a basic report cut short */
	g_oi_len = 5;
	memset(dst, 0xCC, sizeof(dst));
	memset(want, 0xCC, sizeof(want));
	memcpy(want, g_oi_report, (size_t)g_oi_len);
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == 5);
	CHECK(memcmp(dst, want, sizeof(dst)) == 0);
	CHECK(ds45_shim_data.stat_passed == 3);

	/* 4. the DualSense, another report id */
	report_basic(g_oi_report);
	g_oi_report[0] = 0x02;
	g_oi_len = DS_BASIC_LEN;
	memset(dst, 0xCC, sizeof(dst));
	memset(want, 0xCC, sizeof(want));
	memcpy(want, g_oi_report, (size_t)g_oi_len);
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == DS_BASIC_LEN);
	CHECK(memcmp(dst, want, sizeof(dst)) == 0);
	CHECK(ds45_shim_data.stat_passed == 4);

	CHECK(ds45_shim_data.stat_translated == 0);
	CHECK(g_oi_calls == 4);

	/* a buffer smaller than a DS3 report is never rewritten */
	report_basic(g_oi_report);
	g_oi_len = DS_BASIC_LEN;
	memset(dst, 0xCC, sizeof(dst));
	CHECK(shim_input(&g_x_ds, dst, DS3_REPORT_LEN - 1, 0) == DS_BASIC_LEN);
	CHECK(dst[DS_BASIC_LEN] == 0xCC);
	CHECK(ds45_shim_data.stat_translated == 0);

	/* disabled: the original's result, untouched */
	ds45_shim_data.enabled = 0;
	memset(dst, 0xCC, sizeof(dst));
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == DS_BASIC_LEN);
	CHECK(dst[DS_BASIC_LEN] == 0xCC);
	CHECK(ds45_shim_data.stat_translated == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Bounce-buffer input hook (full-report input, output mode 1 or 2)           */
/* ------------------------------------------------------------------------ */

/* A long record that never translates (report id 0x02), recognisable byte by byte. */
static void record_make(uint8_t *r, int n)
{
	int i;

	for (i = 0; i < n; i++)
		r[i] = (uint8_t)(i * 7 + 3);
	r[0] = 0x02;
}

/* The shim's own bounce buffer is untouched (all zero as installed). */
static int inbuf_clean(void)
{
	static const uint8_t zero[sizeof(ds45_shim_data.inbuf)];

	return memcmp(ds45_shim_data.inbuf, zero, sizeof(zero)) == 0;
}

static int bounce_counters_zero(void)
{
	return ds45_shim_data.dbg_in_bounce == 0 && ds45_shim_data.dbg_in_big == 0 &&
	       ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0;
}

/* Mode 0, any other mode value, and a disabled shim never bounce: the original
 * gets the caller's own dst, length and flag, first, as it always did. */
static int test_input_mode0_unchanged(void)
{
	static const uint32_t modes[] = { 0, 3, 99, 0xFFFFFFFFu };
	uint8_t dst[64], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t cfg;
	unsigned i;
	int mode;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		glue_reset();
		ds45_shim_data.out_mode = modes[i];
		cfg = ds45_shim_data.cfg;
		report_basic(g_oi_report);
		g_oi_len = DS_BASIC_LEN;
		CHECK(ds3_report_from_dualsense_basic(g_oi_report, DS_BASIC_LEN, &cfg, want) == 0);
		memset(dst, 0xCC, sizeof(dst));

		CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 7) == DS3_REPORT_LEN);
		CHECK(g_oi_calls == 1);
		CHECK(g_oi_xport == (void *)&g_x_ds && g_oi_dst == dst);
		CHECK(g_oi_len_in == sizeof(dst) && g_oi_flag == 7);
		CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);
		CHECK(dst[DS3_REPORT_LEN] == 0xCC);
		CHECK(ds45_shim_data.stat_translated == 1 && ds45_shim_data.stat_passed == 0);
		CHECK(ds45_shim_data.dbg_input_calls == 1 && ds45_shim_data.dbg_input_ds == 1);
		CHECK(bounce_counters_zero() && inbuf_clean());
	}

	/* a disabled shim in the bounce modes: the original's result, untouched */
	for (mode = 0; mode <= 2; mode++) {
		glue_reset();
		ds45_shim_data.enabled = 0;
		ds45_shim_data.out_mode = (uint32_t)mode;
		report_basic(g_oi_report);
		g_oi_len = DS_BASIC_LEN;
		memset(dst, 0xCC, sizeof(dst));

		CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 7) == DS_BASIC_LEN);
		CHECK(g_oi_calls == 1 && g_oi_dst == dst && g_oi_len_in == sizeof(dst));
		CHECK(g_oi_flag == 7);
		CHECK(memcmp(dst, g_oi_report, DS_BASIC_LEN) == 0);
		CHECK(dst[DS_BASIC_LEN] == 0xCC);
		CHECK(ds45_shim_data.stat_translated == 0 && ds45_shim_data.stat_passed == 0);
		CHECK(ds45_shim_data.dbg_input_calls == 0);
		CHECK(bounce_counters_zero() && inbuf_clean());
	}
	return 0;
}

/* In modes 1 and 2 a DS3's read is bounced like every other (the raised limit is
 * shared, nothing may reach the caller's 64-byte buffer unbounded), but its
 * bytes and return value arrive in the caller's buffer exactly as the original
 * produced them, and nothing is translated. */
static int test_input_ds3_bounced_unchanged(void)
{
	uint8_t dst[96], want[96];
	int mode, i;

	for (mode = 1; mode <= 2; mode++) {
		glue_reset();
		ds45_shim_data.out_mode = (uint32_t)mode;
		for (i = 0; i < RPT_LEN; i++)
			g_oi_report[i] = (uint8_t)(i * 3 + 1);
		g_oi_report[0] = 0x01;
		g_oi_len = DS3_REPORT_LEN;
		memset(dst, 0xCC, sizeof(dst));
		memset(want, 0xCC, sizeof(want));
		memcpy(want, g_oi_report, DS3_REPORT_LEN);

		CHECK(shim_input(&g_x_ds3, dst, 64, 5) == DS3_REPORT_LEN);
		CHECK(g_oi_calls == 1);
		CHECK(g_oi_xport == (void *)&g_x_ds3);
		CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);   /* bounced */
		CHECK(g_oi_flag == 5);
		CHECK(memcmp(dst, want, sizeof(dst)) == 0);       /* byte-identical, no extra */
		CHECK(ds45_shim_data.stat_passed == 1 && ds45_shim_data.stat_translated == 0);
		CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 0);
		CHECK(ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0);
		CHECK(ds45_shim_data.dbg_input_calls == 1 && ds45_shim_data.dbg_input_ds == 0);
		CHECK(ds45_shim_data.dbg_input_padmiss == 0);

		/* a 0x41-byte read fits a 65-byte caller, byte for byte, but is dropped
		 * for a 64-byte one (and counted); more than 0x41 is "big" */
		g_oi_len = 0x41;
		memset(dst, 0xCC, sizeof(dst));
		memset(want, 0xCC, sizeof(want));
		memcpy(want, g_oi_report, 0x41);
		CHECK(shim_input(&g_x_ds3, dst, 0x41, 5) == 0x41);
		CHECK(memcmp(dst, want, sizeof(dst)) == 0);
		CHECK(ds45_shim_data.dbg_in_big == 0 && ds45_shim_data.dbg_in_drop == 0);
		memset(dst, 0xCC, sizeof(dst));
		CHECK(shim_input(&g_x_ds3, dst, 64, 5) == 0);
		CHECK(dst[0] == 0xCC && dst[63] == 0xCC);
		CHECK(ds45_shim_data.dbg_in_bounce == 3 && ds45_shim_data.dbg_in_drop == 1);
		CHECK(ds45_shim_data.stat_translated == 0);
	}
	return 0;
}

/* A DualSense's 78-byte 0x31 report lands in the bounce buffer and comes out as
 * the 49-byte DS3 report in the caller's buffer, and nothing past 49 bytes. */
static int test_input_bounce_translates_full_report(void)
{
	uint8_t buf[96], want[DS3_REPORT_LEN];
	int mode, i;

	ds3_neutral(want);
	for (mode = 1; mode <= 2; mode++) {
		glue_reset();
		ds45_shim_data.out_mode = (uint32_t)mode;
		report_neutral(g_oi_report);
		g_oi_len = RPT_LEN;
		memset(buf, 0xCC, sizeof(buf));

		CHECK(shim_input(&g_x_ds, buf, 64, 3) == DS3_REPORT_LEN);
		CHECK(g_oi_calls == 1);
		CHECK(g_oi_xport == (void *)&g_x_ds);
		CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);
		CHECK(g_oi_flag == 3);
		CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0);
		for (i = DS3_REPORT_LEN; i < 96; i++)
			CHECK(buf[i] == 0xCC);               /* guard bytes, never written */
		CHECK(ds45_shim_data.stat_translated == 1 && ds45_shim_data.stat_passed == 0);
		CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 1);
		CHECK(ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0);
		CHECK(ds45_shim_data.dbg_input_calls == 1 && ds45_shim_data.dbg_input_ds == 1);
	}

	/* a caller that holds exactly a DS3 report */
	glue_reset();
	ds45_shim_data.out_mode = 2;
	report_neutral(g_oi_report);
	g_oi_len = RPT_LEN;
	memset(buf, 0xCC, sizeof(buf));
	CHECK(shim_input(&g_x_ds, buf, DS3_REPORT_LEN, 0) == DS3_REPORT_LEN);
	CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0 && buf[DS3_REPORT_LEN] == 0xCC);
	return 0;
}

/* The 10-byte basic report still translates through the bounce buffer. */
static int test_input_bounce_basic_report(void)
{
	uint8_t buf[96], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t cfg;
	int mode;

	for (mode = 1; mode <= 2; mode++) {
		glue_reset();
		ds45_shim_data.out_mode = (uint32_t)mode;
		cfg = ds45_shim_data.cfg;
		report_basic(g_oi_report);
		g_oi_len = DS_BASIC_LEN;
		CHECK(ds3_report_from_dualsense_basic(g_oi_report, DS_BASIC_LEN, &cfg, want) == 0);
		memset(buf, 0xCC, sizeof(buf));

		CHECK(shim_input(&g_x_ds, buf, 64, 0) == DS3_REPORT_LEN);
		CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);
		CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0);
		CHECK(buf[DS3_REPORT_LEN] == 0xCC);
		CHECK(ds45_shim_data.stat_translated == 1);
		CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 0);
		CHECK(ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0);
	}

	/* too small a caller buffer for the DS3 report: dropped, dst untouched */
	glue_reset();
	ds45_shim_data.out_mode = 2;
	report_basic(g_oi_report);
	g_oi_len = DS_BASIC_LEN;
	memset(buf, 0xCC, sizeof(buf));
	CHECK(shim_input(&g_x_ds, buf, DS3_REPORT_LEN - 1, 0) == 0);
	CHECK(buf[0] == 0xCC && buf[DS3_REPORT_LEN - 1] == 0xCC);
	CHECK(ds45_shim_data.dbg_in_drop == 1 && ds45_shim_data.dbg_in_big == 0);
	return 0;
}

/* Whatever the original reports (1..79 bytes of any content), whichever pad it
 * is (a DualSense, a DS3, a transport no pad owns, a missing pad manager, a pad
 * class that is not set) and however small the caller's buffer, dst is written
 * for at most min(n, len_in) bytes and the return value never exceeds len_in.
 * For every pad but a DualSense the bytes and the count are the original's. */
static int test_input_bounce_never_overruns_dst(void)
{
	uint8_t buf[160];
	void *xport;
	int who, kind, rc, len_in, ret, i, ds;

	for (who = 0; who < 5; who++) {
		for (kind = 0; kind < 3; kind++) {
			for (rc = 1; rc <= 0x4F; rc++) {
				for (len_in = 0; len_in <= 64; len_in++) {
					glue_reset();
					ds45_shim_data.out_mode = (uint32_t)(1 + (rc & 1));
					xport = &g_x_ds;
					ds = who == 0;
					if (who == 1)
						xport = &g_x_ds3;
					else if (who == 2)
						xport = &g_x_other;           /* no pad owns it */
					else if (who == 3)
						ds45_shim_data.pad_mgr = 0;
					else if (who == 4)
						ds45_shim_data.pad_vtable = 0;
					record_make(g_oi_report, 96);
					if (kind == 1)
						report_basic(g_oi_report);
					else if (kind == 2)
						report_neutral(g_oi_report);
					g_oi_len = rc;
					memset(buf, 0xA5, sizeof(buf));

					ret = shim_input(xport, buf + 16, (uint16_t)len_in, 0);

					CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);
					CHECK(ret >= 0 && ret <= len_in);
					for (i = 0; i < 160; i++) {
						if (i < 16 || i >= 16 + ret)
							CHECK(buf[i] == 0xA5);
					}
					if (ds) {
						CHECK(ret == 0 || ret == rc || ret == DS3_REPORT_LEN);
						if (kind == 0 && ret != 0)
							CHECK(memcmp(buf + 16, g_oi_report, (size_t)ret) == 0);
						if (rc > len_in && rc != DS3_REPORT_LEN && ret != 0)
							CHECK(ret == DS3_REPORT_LEN);   /* only a translation shrinks */
					} else {
						/* no translation, ever: the original's bytes and count, or a drop */
						CHECK(ret == (rc <= len_in ? rc : 0));
						CHECK(memcmp(buf + 16, g_oi_report, (size_t)ret) == 0);
						CHECK(ds45_shim_data.stat_translated == 0);
						CHECK(ds45_shim_data.dbg_in_drop == (uint32_t)(rc > len_in));
					}
					CHECK(ds45_shim_data.dbg_in_bounce == 1);
					CHECK(ds45_shim_data.dbg_in_big == (uint32_t)(rc > 0x41));
				}
			}
		}
	}
	return 0;
}

/* An error from the original is returned unchanged and counted only in
 * dbg_in_err (plus the bounce count); a zero-byte read returns 0. */
static int test_input_bounce_error_and_zero(void)
{
	uint8_t dst[64];

	glue_reset();
	ds45_shim_data.out_mode = 2;
	g_oi_len = -5;
	memset(dst, 0xCC, sizeof(dst));
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == -5);
	CHECK(g_oi_calls == 1 && g_oi_dst == ds45_shim_data.inbuf);
	CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_err == 1);
	CHECK(ds45_shim_data.dbg_in_big == 0 && ds45_shim_data.dbg_in_drop == 0);
	CHECK(ds45_shim_data.dbg_input_calls == 0);       /* no valid read to note */
	CHECK(ds45_shim_data.stat_passed == 0 && ds45_shim_data.stat_translated == 0);
	CHECK(dst[0] == 0xCC && dst[63] == 0xCC);

	g_oi_len = 0;
	CHECK(shim_input(&g_x_ds, dst, sizeof(dst), 0) == 0);
	CHECK(ds45_shim_data.dbg_in_bounce == 2 && ds45_shim_data.dbg_in_err == 1);
	CHECK(ds45_shim_data.dbg_in_big == 0 && ds45_shim_data.dbg_in_drop == 0);
	CHECK(dst[0] == 0xCC && dst[63] == 0xCC);

	/* the pure function directly */
	ds45_shim_data = data_enabled();
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, -1) == -1);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, 0) == 0);
	CHECK(shim_input_bounce(NULL, g_pad_ds.b, dst, 64, 10) == 0);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, NULL, 64, 10) == 0);
	CHECK(shim_input_bounce(NULL, g_pad_ds.b, dst, 64, -7) == -7);
	CHECK(dst[0] == 0xCC);
	return 0;
}

/* A 70-byte record that does not translate cannot fit the caller's 64 bytes:
 * dropped (0), dst untouched - from a DualSense, a DS3 and a pad nobody owns. */
static int test_input_bounce_drop_oversize(void)
{
	fake_xport_t *const xp[3] = { &g_x_ds, &g_x_ds3, &g_x_other };
	uint8_t buf[96];
	int i, w;

	for (w = 0; w < 3; w++) {
		glue_reset();
		ds45_shim_data.out_mode = 1;
		record_make(g_oi_report, 70);
		g_oi_len = 70;
		memset(buf, 0xCC, sizeof(buf));

		CHECK(shim_input(xp[w], buf, 64, 0) == 0);
		CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);
		for (i = 0; i < 96; i++)
			CHECK(buf[i] == 0xCC);
		CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 1);
		CHECK(ds45_shim_data.dbg_in_drop == 1 && ds45_shim_data.dbg_in_err == 0);
		CHECK(ds45_shim_data.stat_passed == 1 && ds45_shim_data.stat_translated == 0);
	}
	return 0;
}

/* The same counters from a DS3: every bounce read counts, whichever pad. */
static int test_input_bounce_counters_other_pad(void)
{
	static uint8_t dst[128];
	const ds45_shim_data_t *d = &ds45_shim_data;

	glue_reset();
	ds45_shim_data.out_mode = 1;

#define DS3_READ(len_in) shim_input(&g_x_ds3, dst, (uint16_t)(len_in), 0)

	record_make(g_oi_report, 96);
	g_oi_len = DS3_REPORT_LEN;                   /* 1: a DS3 report */
	CHECK(DS3_READ(64) == 49);
	CHECK(memcmp(dst, g_oi_report, 49) == 0);
	CHECK(d->dbg_in_bounce == 1 && d->dbg_in_big == 0 && d->dbg_in_drop == 0 && d->dbg_in_err == 0);

	g_oi_len = 70;                               /* 2: long, too long for 64 */
	CHECK(DS3_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 2 && d->dbg_in_big == 1 && d->dbg_in_drop == 1 && d->dbg_in_err == 0);

	g_oi_len = -2;                               /* 3: error */
	CHECK(DS3_READ(64) == -2);
	CHECK(d->dbg_in_bounce == 3 && d->dbg_in_big == 1 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	g_oi_len = 0;                                /* 4: nothing */
	CHECK(DS3_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 4 && d->dbg_in_big == 1 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	g_oi_len = 0x41;                             /* 5: 65 fits a 100-byte caller, not "big" */
	CHECK(DS3_READ(100) == 0x41);
	CHECK(d->dbg_in_bounce == 5 && d->dbg_in_big == 1 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	g_oi_len = 0x42;                             /* 6: 66 is big and fits 100 */
	CHECK(DS3_READ(100) == 0x42);
	CHECK(d->dbg_in_bounce == 6 && d->dbg_in_big == 2 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	CHECK(d->stat_translated == 0 && d->stat_passed == 4);   /* 1, 2, 5, 6 */
	CHECK(d->dbg_input_calls == 5 && d->dbg_input_ds == 0);  /* the error was not noted */
#undef DS3_READ
	return 0;
}

/* One read of each kind, with the exact counter values after each. */
static int test_input_bounce_counters(void)
{
	static uint8_t dst[128];
	const ds45_shim_data_t *d = &ds45_shim_data;

	glue_reset();
	ds45_shim_data.out_mode = 2;

#define BOUNCE_READ(len_in) shim_input(&g_x_ds, dst, (uint16_t)(len_in), 0)

	report_basic(g_oi_report);                   /* 1: basic, translates */
	g_oi_len = DS_BASIC_LEN;
	CHECK(BOUNCE_READ(64) == 49);
	CHECK(d->dbg_in_bounce == 1 && d->dbg_in_big == 0 && d->dbg_in_drop == 0 && d->dbg_in_err == 0);

	report_neutral(g_oi_report);                 /* 2: full, translates */
	g_oi_len = RPT_LEN;
	CHECK(BOUNCE_READ(64) == 49);
	CHECK(d->dbg_in_bounce == 2 && d->dbg_in_big == 1 && d->dbg_in_drop == 0 && d->dbg_in_err == 0);

	record_make(g_oi_report, 96);                /* 3: 70 bytes, too long for 64 */
	g_oi_len = 70;
	CHECK(BOUNCE_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 3 && d->dbg_in_big == 2 && d->dbg_in_drop == 1 && d->dbg_in_err == 0);

	g_oi_len = -3;                               /* 4: error */
	CHECK(BOUNCE_READ(64) == -3);
	CHECK(d->dbg_in_bounce == 4 && d->dbg_in_big == 2 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	g_oi_len = 0;                                /* 5: nothing */
	CHECK(BOUNCE_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 5 && d->dbg_in_big == 2 && d->dbg_in_drop == 1 && d->dbg_in_err == 1);

	g_oi_len = 0x41;                             /* 6: 65 is not "big", but 65 > 64 */
	CHECK(BOUNCE_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 6 && d->dbg_in_big == 2 && d->dbg_in_drop == 2 && d->dbg_in_err == 1);

	g_oi_len = 0x42;                             /* 7: 66 is big, and fits a 100-byte caller */
	CHECK(BOUNCE_READ(100) == 0x42);
	CHECK(memcmp(dst, g_oi_report, 0x42) == 0);
	CHECK(d->dbg_in_bounce == 7 && d->dbg_in_big == 3 && d->dbg_in_drop == 2 && d->dbg_in_err == 1);

	g_oi_len = 0x4F;                             /* 8: the longest read, caller of 64 */
	CHECK(BOUNCE_READ(64) == 0);
	CHECK(d->dbg_in_bounce == 8 && d->dbg_in_big == 4 && d->dbg_in_drop == 3 && d->dbg_in_err == 1);

	g_oi_len = 0x42;                             /* 9: big, one byte short of fitting */
	CHECK(BOUNCE_READ(0x41) == 0);
	CHECK(d->dbg_in_bounce == 9 && d->dbg_in_big == 5 && d->dbg_in_drop == 4 && d->dbg_in_err == 1);

	/* 10: more than the read can ever return (defensive): dropped, not "big" */
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, 0x50) == 0);
	CHECK(d->dbg_in_bounce == 10 && d->dbg_in_big == 5 && d->dbg_in_drop == 5 && d->dbg_in_err == 1);

	CHECK(d->stat_translated == 2 && d->stat_passed == 5);
	CHECK(d->dbg_input_calls == 9 && d->dbg_input_ds == 9);   /* the error was not noted */
#undef BOUNCE_READ
	return 0;
}

/* The pad is looked up AFTER the bounce read. A lookup miss (a transport no pad
 * owns), a pad that is not a DualSense, an unreachable pad manager and an unset
 * pad class all take the bounded pass-through: the read is bounced, the bytes
 * reach the caller untranslated and counted exactly, never an overrun. NULL
 * arguments keep the normal path with the caller's own buffer. */
static int test_input_bounce_pad_lookup(void)
{
	uint8_t dst[64], want[64];
	fake_pad_t pad4;
	int what;

	for (what = 0; what < 6; what++) {
		glue_reset();
		ds45_shim_data.out_mode = 2;
		pad_make(&pad4, PAD_VTABLE, VID_SONY, PID_DS4);
		put_native(pad4.b + SHIM_PAD_XPORT, &g_x_other);
		put_native(g_mgr.b + SHIM_MGR_OBJS + 7 * 8, pad4.b);
		report_basic(g_oi_report);
		g_oi_len = DS_BASIC_LEN;
		memset(dst, 0xCC, sizeof(dst));
		memset(want, 0xCC, sizeof(want));
		memcpy(want, g_oi_report, DS_BASIC_LEN);

		if (what <= 3) {
			void *xport = &g_x_other;

			if (what == 0)                       /* no pad owns this transport */
				put_native(g_mgr.b + SHIM_MGR_OBJS + 7 * 8, NULL);
			else if (what == 2) {                /* no pad manager */
				ds45_shim_data.pad_mgr = 0;
				xport = &g_x_ds;
			} else if (what == 3) {              /* the pad class is not set */
				ds45_shim_data.pad_vtable = 0;
				xport = &g_x_ds;
			}                                    /* what == 1: a pad that is a DS4 */

			CHECK(shim_input(xport, dst, sizeof(dst), 9) == DS_BASIC_LEN);
			CHECK(g_oi_calls == 1 && g_oi_xport == xport);
			CHECK(g_oi_dst == ds45_shim_data.inbuf && g_oi_len_in == 0x50);   /* bounced */
			CHECK(g_oi_flag == 9);
			CHECK(memcmp(dst, want, sizeof(dst)) == 0);   /* untranslated, as read */
			CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 0);
			CHECK(ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0);
			CHECK(ds45_shim_data.stat_translated == 0 && ds45_shim_data.stat_passed == 1);
			CHECK(ds45_shim_data.dbg_input_calls == 1 && ds45_shim_data.dbg_input_ds == 0);
			CHECK(ds45_shim_data.dbg_input_padmiss == (what == 1 ? 0u : (what == 3 ? 0u : 1u)));
			if (what == 1)
				CHECK(ds45_shim_data.dbg_last_pid == 0x054C05C4u);

			/* a long record from the same pad cannot fit 64 bytes: dropped, counted */
			g_oi_len = 0x4F;
			record_make(g_oi_report, 0x4F);
			memset(dst, 0xCC, sizeof(dst));
			CHECK(shim_input(xport, dst, sizeof(dst), 9) == 0);
			CHECK(dst[0] == 0xCC && dst[63] == 0xCC);
			CHECK(ds45_shim_data.dbg_in_bounce == 2 && ds45_shim_data.dbg_in_big == 1);
			CHECK(ds45_shim_data.dbg_in_drop == 1 && ds45_shim_data.dbg_in_err == 0);
		} else if (what == 4) {                  /* NULL transport: the normal path */
			CHECK(shim_input(NULL, dst, sizeof(dst), 9) == DS_BASIC_LEN);
			CHECK(g_oi_xport == NULL && g_oi_dst == dst);
			CHECK(g_oi_calls == 1 && g_oi_len_in == sizeof(dst) && g_oi_flag == 9);
			CHECK(memcmp(dst, want, sizeof(dst)) == 0);
			CHECK(ds45_shim_data.stat_translated == 0);
			CHECK(bounce_counters_zero() && inbuf_clean());
		} else {                                 /* NULL destination: the normal path */
			CHECK(shim_input(&g_x_ds, NULL, sizeof(dst), 9) == DS_BASIC_LEN);
			CHECK(g_oi_xport == (void *)&g_x_ds && g_oi_dst == NULL);
			CHECK(g_oi_calls == 1 && g_oi_len_in == sizeof(dst) && g_oi_flag == 9);
			CHECK(bounce_counters_zero() && inbuf_clean());
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Raw DualSense report capture (version 11, debug only)                      */
/* ------------------------------------------------------------------------ */

/* The raw capture is not part of the bounce counters: it must not disturb them. */
static int raw_capture_clean(void)
{
	static const uint8_t zero[sizeof(ds45_shim_data.dbg_in_raw)];

	return ds45_shim_data.dbg_in_raw_len == 0 && ds45_shim_data.dbg_in_raw_seq == 0 &&
	       memcmp(ds45_shim_data.dbg_in_raw, zero, sizeof(zero)) == 0;
}

/* A neutral 0x31 report with recognisable bytes in the common block, so a copy
 * is visible byte for byte (the translation does not refuse them). */
static void report_marked(uint8_t r[RPT_LEN])
{
	int i;

	report_neutral(r);
	for (i = BT(DS_C_STATUS) - 4; i < BT(DS_C_STATUS) + 8; i++)
		r[i] = (uint8_t)(0xA0 + i);
	r[BT(DS_C_STATUS)] = 0x2A;
}

/* A 78-byte 0x31 report: the first 64 bytes arrive in dbg_in_raw exactly as the
 * original wrote them, before the in-place translation, and the translated
 * 49-byte report still reaches dst unchanged. */
static int test_input_raw_capture_full_report(void)
{
	uint8_t dst[96], want[DS3_REPORT_LEN], raw[RPT_LEN];
	ds45_shim_cfg_t cfg;
	int mode, i;

	for (mode = 1; mode <= 2; mode++) {
		glue_reset();
		ds45_shim_data.out_mode = (uint32_t)mode;
		cfg = ds45_shim_data.cfg;
		report_marked(g_oi_report);
		memcpy(raw, g_oi_report, RPT_LEN);
		g_oi_len = RPT_LEN;
		CHECK(ds3_report_from_dualsense(raw, RPT_LEN, &cfg, want) == 0);
		memset(dst, 0xCC, sizeof(dst));
		CHECK(raw_capture_clean());

		CHECK(shim_input(&g_x_ds, dst, 64, 0) == DS3_REPORT_LEN);
		CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);       /* translation unchanged */
		CHECK(dst[DS3_REPORT_LEN] == 0xCC);
		CHECK(ds45_shim_data.dbg_in_raw_len == 64);
		CHECK(ds45_shim_data.dbg_in_raw_seq == 1);
		CHECK(memcmp(ds45_shim_data.dbg_in_raw, g_oi_report, 64) == 0);
		CHECK(ds45_shim_data.dbg_in_raw[BT(DS_C_STATUS)] == 0x2A);   /* byte 54, raw */
		CHECK(ds45_shim_data.inbuf[0] == 0x01);              /* inbuf holds the translated report */
		for (i = 0; i < 64; i++)
			CHECK(ds45_shim_data.dbg_in_raw[i] == raw[i]);
		/* the existing counters are exactly as before */
		CHECK(ds45_shim_data.stat_translated == 1 && ds45_shim_data.stat_passed == 0);
		CHECK(ds45_shim_data.dbg_in_bounce == 1 && ds45_shim_data.dbg_in_big == 1);
		CHECK(ds45_shim_data.dbg_in_drop == 0 && ds45_shim_data.dbg_in_err == 0);
	}

	/* a 79-byte read (the longest) is clamped to the same 64 bytes */
	glue_reset();
	ds45_shim_data.out_mode = 2;
	report_marked(g_oi_report);
	g_oi_report[78] = 0x77;
	g_oi_len = 0x4F;
	CHECK(shim_input(&g_x_ds, dst, 100, 0) > 0);
	CHECK(ds45_shim_data.dbg_in_raw_len == 64 && ds45_shim_data.dbg_in_raw_seq == 1);
	CHECK(memcmp(ds45_shim_data.dbg_in_raw, g_oi_report, 64) == 0);
	return 0;
}

/* A 10-byte basic report: the capture holds those 10 bytes, length 10, and the
 * bytes after them are not touched. */
static int test_input_raw_capture_basic_report(void)
{
	uint8_t dst[96], want[DS3_REPORT_LEN], raw[DS_BASIC_LEN];
	ds45_shim_cfg_t cfg;
	int i;

	glue_reset();
	ds45_shim_data.out_mode = 2;
	cfg = ds45_shim_data.cfg;
	report_basic(g_oi_report);
	memcpy(raw, g_oi_report, DS_BASIC_LEN);
	g_oi_len = DS_BASIC_LEN;
	CHECK(ds3_report_from_dualsense_basic(raw, DS_BASIC_LEN, &cfg, want) == 0);
	memset(dst, 0xCC, sizeof(dst));

	CHECK(shim_input(&g_x_ds, dst, 64, 0) == DS3_REPORT_LEN);
	CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);
	CHECK(ds45_shim_data.dbg_in_raw_len == DS_BASIC_LEN);
	CHECK(ds45_shim_data.dbg_in_raw_seq == 1);
	CHECK(memcmp(ds45_shim_data.dbg_in_raw, raw, DS_BASIC_LEN) == 0);
	for (i = DS_BASIC_LEN; i < 64; i++)
		CHECK(ds45_shim_data.dbg_in_raw[i] == 0);

	/* a shorter report after a longer one: the length shrinks, the stale tail
	 * is allowed to stay (the length says what is valid) */
	report_marked(g_oi_report);
	g_oi_len = RPT_LEN;
	CHECK(shim_input(&g_x_ds, dst, 64, 0) == DS3_REPORT_LEN);
	CHECK(ds45_shim_data.dbg_in_raw_len == 64 && ds45_shim_data.dbg_in_raw_seq == 2);
	report_basic(g_oi_report);
	g_oi_len = DS_BASIC_LEN;
	CHECK(shim_input(&g_x_ds, dst, 64, 0) == DS3_REPORT_LEN);
	CHECK(ds45_shim_data.dbg_in_raw_len == DS_BASIC_LEN && ds45_shim_data.dbg_in_raw_seq == 3);
	CHECK(memcmp(ds45_shim_data.dbg_in_raw, raw, DS_BASIC_LEN) == 0);
	return 0;
}

/* Only a DualSense read of 1..0x4F bytes is captured: not a DS3, not a pad nobody
 * owns, not an error, a zero-byte read or an impossible length. */
static int test_input_raw_capture_only_dualsense(void)
{
	fake_xport_t *const xp[2] = { &g_x_ds3, &g_x_other };
	uint8_t dst[128];
	int w;

	for (w = 0; w < 2; w++) {
		glue_reset();
		ds45_shim_data.out_mode = 1;
		record_make(g_oi_report, 70);
		g_oi_len = 70;
		CHECK(shim_input(xp[w], dst, 100, 0) == 70);
		g_oi_len = DS_BASIC_LEN;
		report_basic(g_oi_report);
		CHECK(shim_input(xp[w], dst, 100, 0) == DS_BASIC_LEN);
		g_oi_len = DS3_REPORT_LEN;
		CHECK(shim_input(xp[w], dst, 100, 0) == DS3_REPORT_LEN);
		CHECK(ds45_shim_data.dbg_in_bounce == 3);
		CHECK(raw_capture_clean());
	}

	/* a DualSense, but an error, nothing, or more than the read can return */
	glue_reset();
	ds45_shim_data.out_mode = 2;
	record_make(g_oi_report, 96);
	g_oi_len = -4;
	CHECK(shim_input(&g_x_ds, dst, 64, 0) == -4);
	CHECK(raw_capture_clean());
	g_oi_len = 0;
	CHECK(shim_input(&g_x_ds, dst, 64, 0) == 0);
	CHECK(raw_capture_clean());
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, -1) == -1);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, 0) == 0);
	memcpy(ds45_shim_data.inbuf, g_oi_report, 96);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, 0x50) == 0);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, dst, 64, 0x7FFFFFFF) == 0);
	CHECK(raw_capture_clean());
	CHECK(ds45_shim_data.dbg_in_drop == 2);

	/* NULL arguments change nothing either */
	CHECK(shim_input_bounce(NULL, g_pad_ds.b, dst, 64, 10) == 0);
	CHECK(shim_input_bounce(&ds45_shim_data, g_pad_ds.b, NULL, 64, 10) == 0);
	CHECK(raw_capture_clean());

	/* one DualSense read of 1 byte is captured (the lower edge) */
	g_oi_len = 1;
	CHECK(shim_input(&g_x_ds, dst, 64, 0) == 1);
	CHECK(ds45_shim_data.dbg_in_raw_len == 1 && ds45_shim_data.dbg_in_raw_seq == 1);
	CHECK(ds45_shim_data.dbg_in_raw[0] == g_oi_report[0]);
	return 0;
}

/* Each captured read bumps the sequence; the latest bytes are held. */
static int test_input_raw_capture_seq_counts(void)
{
	uint8_t dst[96];
	int k;

	glue_reset();
	ds45_shim_data.out_mode = 2;
	for (k = 1; k <= 3; k++) {
		report_marked(g_oi_report);
		g_oi_report[BT(DS_C_STATUS)] = (uint8_t)(0x10 * k + k);
		g_oi_len = RPT_LEN;
		CHECK(shim_input(&g_x_ds, dst, 64, 0) == DS3_REPORT_LEN);
		CHECK(ds45_shim_data.dbg_in_raw_seq == (uint32_t)k);
		CHECK(ds45_shim_data.dbg_in_raw_len == 64);
		CHECK(ds45_shim_data.dbg_in_raw[BT(DS_C_STATUS)] == (uint8_t)(0x10 * k + k));
		CHECK(memcmp(ds45_shim_data.dbg_in_raw, g_oi_report, 64) == 0);
	}
	CHECK(ds45_shim_data.dbg_in_bounce == 3 && ds45_shim_data.stat_translated == 3);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Diagnostic counters (debug tracing, Amendment 3)                           */
/* ------------------------------------------------------------------------ */

/* The event hook counts every entry past its guard in dbg_event_calls, and a
 * DualSense match in dbg_event_ds; a DS3 (which passes to the original) bumps
 * dbg_event_calls only, and a disabled shim counts nothing. */
static int test_dbg_counts_event(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_drv_t drv;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	CHECK(shim_event_complete(&d, drv.b) == 1);
	CHECK(d.dbg_event_calls == 1);
	CHECK(d.dbg_event_ds == 1);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	drv_make(&drv, &pad);
	CHECK(shim_event_complete(&d, drv.b) == 0);
	CHECK(d.dbg_event_calls == 2);
	CHECK(d.dbg_event_ds == 1);                 /* not a DualSense: unchanged */

	/* the enable counters belong to the other hook */
	CHECK(d.dbg_enable_calls == 0 && d.dbg_enable_ds == 0);

	/* a disabled shim returns from the guard before counting */
	d.enabled = 0;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&drv, &pad);
	CHECK(shim_event_complete(&d, drv.b) == 0);
	CHECK(d.dbg_event_calls == 2 && d.dbg_event_ds == 1);
	return 0;
}

/* The enable hook counts the same way: dbg_enable_calls on every entry,
 * dbg_enable_ds on a DualSense, a DS3 only calls, a disabled shim nothing. */
static int test_dbg_counts_enable(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	fake_en_t en;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	en_make(&en, &pad);
	CHECK(shim_enable_complete(&d, en.b) == 1);
	CHECK(d.dbg_enable_calls == 1);
	CHECK(d.dbg_enable_ds == 1);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	en_make(&en, &pad);
	CHECK(shim_enable_complete(&d, en.b) == 0);
	CHECK(d.dbg_enable_calls == 2);
	CHECK(d.dbg_enable_ds == 1);

	CHECK(d.dbg_event_calls == 0 && d.dbg_event_ds == 0);

	d.enabled = 0;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	en_make(&en, &pad);
	CHECK(shim_enable_complete(&d, en.b) == 0);
	CHECK(d.dbg_enable_calls == 2 && d.dbg_enable_ds == 1);
	return 0;
}

/* shim_input_note records what the input hook saw: it always bumps
 * dbg_input_calls; a NULL pad bumps dbg_input_padmiss and nothing else; a real
 * pad records its vtable and big-endian VID||PID, and a DualSense bumps
 * dbg_input_ds. A NULL data block is a no-op. */
static int test_input_note(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;

	shim_input_note(NULL, NULL);                /* never crashes, counts nothing */

	shim_input_note(&d, NULL);
	CHECK(d.dbg_input_calls == 1);
	CHECK(d.dbg_input_padmiss == 1);
	CHECK(d.dbg_input_ds == 0);
	CHECK(d.dbg_last_pid == 0 && d.dbg_last_vtable == 0);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	shim_input_note(&d, pad.b);
	CHECK(d.dbg_input_calls == 2);
	CHECK(d.dbg_input_padmiss == 1);            /* a real pad is not a miss */
	CHECK(d.dbg_input_ds == 0);                 /* not a DualSense */
	CHECK(d.dbg_last_pid == 0x054C0268u);
	CHECK(d.dbg_last_vtable == PAD_VTABLE);

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	shim_input_note(&d, pad.b);
	CHECK(d.dbg_input_calls == 3);
	CHECK(d.dbg_input_padmiss == 1);
	CHECK(d.dbg_input_ds == 1);
	CHECK(d.dbg_last_pid == 0x054C0CE6u);
	CHECK(d.dbg_last_vtable == PAD_VTABLE);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Stable identity from the Bluetooth connection (stage 1b)                   */
/* ------------------------------------------------------------------------ */

#define CONN_LEN    0x60
#define CONN_HANDLES 7

typedef union { uint64_t q[CONN_LEN / 8]; uint8_t b[CONN_LEN]; } fake_conn_t;

/* A connection entry for `handle` whose +0x08..+0x0F hold a controller's
 * Bluetooth address in the pad-identity layout (u32 @+8, byte @+0xC, an unused
 * byte @+0xD that is deliberately non-zero junk here, u16 @+0xE). */
static void conn_make(fake_conn_t *c, uint8_t handle, const uint8_t id[8])
{
	memset(c->b, 0x77, sizeof(c->b));
	memcpy(c->b + 8, id, 8);
	c->b[0x10] = handle;
}

/* The connection table: 7 native pointers. A data block pointed at it with the
 * stable identity switched on. */
static uint64_t g_ctbl[CONN_HANDLES];

static ds45_shim_data_t data_stable(void)
{
	ds45_shim_data_t d = data_enabled();

	memset(g_ctbl, 0, sizeof(g_ctbl));
	d.conn_tbl = (uint64_t)(uintptr_t)g_ctbl;
	d.stable_id = 1;
	return d;
}

static void drv_handle(fake_drv_t *drv, uint8_t h)
{
	drv->b[SHIM_DRV_HANDLE] = h;
}

/* Two controllers on handles 0 and 1: each pad's identity bytes are exactly its
 * connection's address (the unused byte +0x0D forced to 0), distinct from each
 * other, and the fixed-tag counter form is not used. */
static int test_stable_identity_from_connection(void)
{
	ds45_shim_data_t d = data_stable();
	fake_conn_t ca, cb;
	fake_pad_t pa, pb;
	fake_drv_t da, db;
	static const uint8_t bd_a[8] = { 0x00, 0x13, 0x24, 0xA8, 0x9B, 0x64, 0x4C, 0xB9 };
	static const uint8_t bd_b[8] = { 0x00, 0xA6, 0xC9, 0x13, 0x51, 0x00, 0xA0, 0xAB };
	static const uint8_t want_a[8] = { 0x00, 0x13, 0x24, 0xA8, 0x9B, 0x00, 0x4C, 0xB9 };
	static const uint8_t want_b[8] = { 0x00, 0xA6, 0xC9, 0x13, 0x51, 0x00, 0xA0, 0xAB };

	conn_make(&ca, 0, bd_a);
	conn_make(&cb, 1, bd_b);
	g_ctbl[0] = (uint64_t)(uintptr_t)ca.b;
	g_ctbl[1] = (uint64_t)(uintptr_t)cb.b;

	pad_make(&pa, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	pad_make(&pb, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&da, &pa);
	drv_make(&db, &pb);
	drv_handle(&da, 0);
	drv_handle(&db, 1);

	CHECK(shim_event_complete(&d, da.b) == 1);
	CHECK(shim_event_complete(&d, db.b) == 1);
	CHECK(memcmp(pa.b + 0x88, want_a, 8) == 0);
	CHECK(memcmp(pb.b + 0x88, want_b, 8) == 0);
	CHECK(memcmp(pa.b + 0x88, pb.b + 0x88, 8) != 0);

	/* the template outside the identity is still copied, and the driver is done */
	CHECK(memcmp(pa.b + 0x90, d.info_tmpl + 0x10, DS45_SHIM_INFO_LEN - 0x10) == 0);
	CHECK(da.b[SHIM_DRV_SUBSTATE] == SHIM_DRV_DONE);

	CHECK(d.dbg_id_bd == 2 && d.dbg_id_fallback == 0);
	CHECK(d.dbg_last_handle == 1);
	CHECK(d.stat_completed == 2);               /* still counts every completion */
	/* the connection entries were only read */
	CHECK(ca.b[0x08] == 0x00 && ca.b[0x0D] == 0x64 && cb.b[0x10] == 1);
	return 0;
}

/* The same controller reconnecting on a new pad object (even a different handle
 * and a different completion count) gets the identical identity. */
static int test_stable_identity_survives_reconnect(void)
{
	ds45_shim_data_t d = data_stable();
	fake_conn_t c1, c2;
	fake_pad_t p1, p2;
	fake_drv_t d1, d2;
	static const uint8_t bd[8] = { 0x00, 0x13, 0x24, 0xA8, 0x9B, 0x64, 0x4C, 0xB9 };

	conn_make(&c1, 0, bd);
	g_ctbl[0] = (uint64_t)(uintptr_t)c1.b;
	pad_make(&p1, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&d1, &p1);
	drv_handle(&d1, 0);
	CHECK(shim_event_complete(&d, d1.b) == 1);

	/* gone, then back on handle 3 */
	g_ctbl[0] = 0;
	conn_make(&c2, 3, bd);
	g_ctbl[3] = (uint64_t)(uintptr_t)c2.b;
	pad_make(&p2, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	drv_make(&d2, &p2);
	drv_handle(&d2, 3);
	CHECK(shim_event_complete(&d, d2.b) == 1);

	CHECK(memcmp(p1.b + 0x88, p2.b + 0x88, 8) == 0);
	CHECK(d.stat_completed == 2);
	CHECK(d.dbg_id_bd == 2 && d.dbg_id_fallback == 0);
	return 0;
}

/* One completion with the stable identity requested, whose connection lookup is
 * set up by `setup`, must fall back to the counter form
 * (stat_completed, 0xD5, 0x00, 0x44, 0x53) and count the fallback. */
static int expect_fallback(ds45_shim_data_t *d, fake_drv_t *drv, fake_pad_t *pad)
{
	static const uint8_t id1[8] = { 0x00, 0x00, 0x00, 0x01, 0xD5, 0x00, 0x44, 0x53 };

	CHECK(shim_event_complete(d, drv->b) == 1);
	CHECK(memcmp(pad->b + 0x88, id1, 8) == 0);
	CHECK(d->stat_completed == 1);
	CHECK(d->dbg_id_bd == 0 && d->dbg_id_fallback == 1);
	CHECK(drv->b[SHIM_DRV_SUBSTATE] == SHIM_DRV_DONE);
	return 0;
}

static int test_stable_identity_fallbacks(void)
{
	static const uint8_t bd[8] = { 0x00, 0x13, 0x24, 0xA8, 0x9B, 0x64, 0x4C, 0xB9 };
	static const uint8_t zero[8] = { 0 };
	/* +0x8C == 0x18 with the u16 at +0x8E == 0x000D: the kernel's alternate branch */
	static const uint8_t alt[8] = { 0x00, 0x13, 0x24, 0xA8, 0x18, 0x00, 0x00, 0x0D };
	ds45_shim_data_t d;
	fake_conn_t c;
	fake_pad_t pad;
	fake_drv_t drv;
	int r;

	/* handle past the table */
	d = data_stable(); conn_make(&c, 0, bd); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 7);
	if ((r = expect_fallback(&d, &drv, &pad)) != 0) return r;

	/* an empty slot */
	d = data_stable(); conn_make(&c, 0, bd); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 2);
	if ((r = expect_fallback(&d, &drv, &pad)) != 0) return r;

	/* the entry's own handle byte does not match the slot it was found in */
	d = data_stable(); conn_make(&c, 5, bd); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 0);
	if ((r = expect_fallback(&d, &drv, &pad)) != 0) return r;

	/* an all-zero address */
	d = data_stable(); conn_make(&c, 0, zero); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 0);
	if ((r = expect_fallback(&d, &drv, &pad)) != 0) return r;

	/* the alternate-branch pattern */
	d = data_stable(); conn_make(&c, 0, alt); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 0);
	if ((r = expect_fallback(&d, &drv, &pad)) != 0) return r;
	return 0;
}

/* Off by configuration (stable_id 0) or no table address: the counter form, and
 * nothing is counted as a fallback because the lookup was never tried. */
static int test_stable_identity_off(void)
{
	static const uint8_t bd[8] = { 0x00, 0x13, 0x24, 0xA8, 0x9B, 0x64, 0x4C, 0xB9 };
	static const uint8_t id1[8] = { 0x00, 0x00, 0x00, 0x01, 0xD5, 0x00, 0x44, 0x53 };
	ds45_shim_data_t d;
	fake_conn_t c;
	fake_pad_t pad;
	fake_drv_t drv;

	d = data_stable(); d.stable_id = 0;
	conn_make(&c, 0, bd); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 0);
	CHECK(shim_event_complete(&d, drv.b) == 1);
	CHECK(memcmp(pad.b + 0x88, id1, 8) == 0);
	CHECK(d.dbg_id_bd == 0 && d.dbg_id_fallback == 0);

	d = data_stable(); d.conn_tbl = 0;
	conn_make(&c, 0, bd); g_ctbl[0] = (uint64_t)(uintptr_t)c.b;
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE); drv_make(&drv, &pad);
	drv_handle(&drv, 0);
	CHECK(shim_event_complete(&d, drv.b) == 1);
	CHECK(memcmp(pad.b + 0x88, id1, 8) == 0);
	CHECK(d.dbg_id_bd == 0 && d.dbg_id_fallback == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* DS3-route output hook (observe only): shim_output_filter                   */
/* ------------------------------------------------------------------------ */

/* The kernel's output call hands the filter the payload at pad+0x22B, 48 bytes
 * of it; a payload pattern makes the byte-for-byte capture visible. */
static void out_payload(fake_pad_t *pad, int n, uint8_t seed)
{
	int i;

	for (i = 0; i < n; i++)
		pad->b[SHIM_PAD_OUT_BUF_OFF + i] = (uint8_t)(seed + i * 3);
}

/* Sentinels for the three out-parameters: the observe-only filter must leave
 * all of them exactly as they were. */
#define RID_SENTINEL 0xA7
#define OLEN_SENTINEL 0xBEEF

static int test_output_counts_and_captures(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;

	/* a DualSense pad, 48-byte payload */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	out_payload(&pad, 48, 0x10);
	CHECK(SHIM_PAD_OUT_BUF_OFF == 0x22B);
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_calls == 1);
	CHECK(d.dbg_out_ds == 1);
	CHECK(d.dbg_out_passed == 1);
	CHECK(d.dbg_out_translated == 0);
	CHECK(d.dbg_out_last_len == 48);
	CHECK(d.dbg_out_cap_len == 48);
	CHECK(memcmp(d.dbg_out_cap, pad.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	/* observe only: no output parameter is written, the payload is not touched */
	CHECK(rid == RID_SENTINEL);
	CHECK(ob == (const uint8_t *)&pad);
	CHECK(ol == OLEN_SENTINEL);
	CHECK(pad.b[SHIM_PAD_OUT_BUF_OFF + 5] == (uint8_t)(0x10 + 15));
	CHECK(pad.b[SHIM_PAD_OUT_BUF_OFF + 48] == 0x5A);
	/* nothing else of the data block moved */
	CHECK(d.stat_translated == 0 && d.stat_passed == 0 && d.out_seq == 0);

	/* a second call bumps again */
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_calls == 2 && d.dbg_out_ds == 2 && d.dbg_out_passed == 2);
	return 0;
}

/* A DualShock 3: the Sony pad class but PID 0x0268. It is counted and captured
 * (the capture is the ground truth about what the console sends a DS3), but not
 * as a DualSense, and nothing is rewritten. */
static int test_output_ds3_pad_untouched(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad, before;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
	out_payload(&pad, 48, 0x40);
	before = pad;
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_calls == 1);
	CHECK(d.dbg_out_ds == 0);
	CHECK(d.dbg_out_passed == 1);
	CHECK(d.dbg_out_last_len == 48 && d.dbg_out_cap_len == 48);
	CHECK(memcmp(d.dbg_out_cap, pad.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
	CHECK(memcmp(pad.b, before.b, sizeof(pad.b)) == 0);

	/* a pad of another class entirely (the vtable does not match): same */
	pad_make(&pad, PAD_VTABLE + 8, VID_SONY, PID_DUALSENSE);
	out_payload(&pad, 48, 0x40);
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_calls == 2 && d.dbg_out_ds == 0 && d.dbg_out_passed == 2);
	return 0;
}

static int test_output_bad_arguments(void)
{
	ds45_shim_data_t d = data_enabled(), zero;
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;
	const uint8_t *buf;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	buf = pad.b + SHIM_PAD_OUT_BUF_OFF;

	/* NULL data block: return 0, write nothing (the call must not crash) */
	CHECK(shim_output_filter(NULL, buf, 48, &rid, &ob, &ol) == 0);
	CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);

	/* NULL buffer: counted as a call, nothing else */
	zero = d;
	CHECK(shim_output_filter(&d, NULL, 48, &rid, &ob, &ol) == 0);
	zero.dbg_out_calls = 1;
	CHECK(memcmp(&d, &zero, sizeof(d)) == 0);

	/* length 0 */
	CHECK(shim_output_filter(&d, buf, 0, &rid, &ob, &ol) == 0);
	zero.dbg_out_calls = 2;
	CHECK(memcmp(&d, &zero, sizeof(d)) == 0);

	/* length 65, over the 64-byte payload cap */
	CHECK(shim_output_filter(&d, buf, 65, &rid, &ob, &ol) == 0);
	zero.dbg_out_calls = 3;
	CHECK(memcmp(&d, &zero, sizeof(d)) == 0);
	CHECK(d.dbg_out_ds == 0 && d.dbg_out_passed == 0 && d.dbg_out_cap_len == 0);

	/* shim disabled: counted as a call, nothing captured, nothing else */
	d = data_enabled();
	d.enabled = 0;
	zero = d;
	CHECK(shim_output_filter(&d, buf, 48, &rid, &ob, &ol) == 0);
	zero.dbg_out_calls = 1;
	CHECK(memcmp(&d, &zero, sizeof(d)) == 0);

	/* none of the above wrote an out-parameter or the payload */
	CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
	return 0;
}

/* The capture holds the LAST payload only, and its length is the payload length
 * (at most 64, the cap the kernel's sender enforces). */
static int test_output_capture_keeps_last(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t pad;
	uint8_t rid = 0;
	const uint8_t *ob = NULL;
	uint16_t ol = 0;
	const uint8_t *buf;
	int i;

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	buf = pad.b + SHIM_PAD_OUT_BUF_OFF;

	/* the longest payload the kernel accepts: all 64 bytes captured */
	out_payload(&pad, 64, 0x01);
	CHECK(shim_output_filter(&d, buf, 64, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_last_len == 64 && d.dbg_out_cap_len == 64);
	CHECK(memcmp(d.dbg_out_cap, buf, 64) == 0);

	/* a shorter, different one replaces it: the length follows, the bytes are
	 * the new ones, nothing past the length is claimed */
	out_payload(&pad, 30, 0xC0);
	CHECK(shim_output_filter(&d, buf, 30, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_last_len == 30 && d.dbg_out_cap_len == 30);
	CHECK(memcmp(d.dbg_out_cap, buf, 30) == 0);
	for (i = 0; i < 30; i++)
		CHECK(d.dbg_out_cap[i] == (uint8_t)(0xC0 + i * 3));

	/* a rejected call (65) does not disturb the capture */
	CHECK(shim_output_filter(&d, buf, 65, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_last_len == 30 && d.dbg_out_cap_len == 30);
	CHECK(d.dbg_out_calls == 3);

	/* one byte */
	CHECK(shim_output_filter(&d, buf, 1, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_last_len == 1 && d.dbg_out_cap_len == 1);
	CHECK(d.dbg_out_cap[0] == buf[0]);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Rumble-aware capture: the last motor-on payload and the next zero-motor    */
/* payload from the same pad (debug only; the filter still returns 0).        */
/* ------------------------------------------------------------------------ */

/* A pattern payload of n bytes (n >= 5) with the two motor bytes set: payload
 * byte 2 is the small motor on/off, byte 4 the large motor strength. */
static void out_motor(fake_pad_t *pad, int n, uint8_t seed, uint8_t small, uint8_t large)
{
	out_payload(pad, n, seed);
	pad->b[SHIM_PAD_OUT_BUF_OFF + 2] = small;
	pad->b[SHIM_PAD_OUT_BUF_OFF + 4] = large;
}

static uint64_t pad_key(const fake_pad_t *pad)
{
	return (uint64_t)(uintptr_t)pad->b;
}

static int test_output_motor_on_capture(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t a, b, c;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = (const uint8_t *)&a;
	uint16_t ol = OLEN_SENTINEL;
	const uint8_t zero64[64] = {0};

	pad_make(&a, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	pad_make(&b, PAD_VTABLE, VID_SONY, PID_DS3);
	pad_make(&c, PAD_VTABLE, VID_SONY, PID_DUALSENSE);

	/* nothing captured at the start */
	CHECK(d.dbg_out_on_count == 0 && d.dbg_out_on_len == 0 && d.dbg_out_on_pad == 0);
	CHECK(d.dbg_out_off_count == 0 && d.dbg_out_off_len == 0 && d.dbg_out_off_armed == 0);

	/* a DualSense pad, small motor on: captured exactly */
	out_motor(&a, 48, 0x10, 0x01, 0x00);
	CHECK(shim_output_filter(&d, a.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 1);
	CHECK(d.dbg_out_on_len == 48);
	CHECK(d.dbg_out_on_pad == pad_key(&a));
	CHECK(memcmp(d.dbg_out_on_cap, a.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	CHECK(d.dbg_out_off_armed == 1);
	CHECK(d.dbg_out_off_count == 0 && d.dbg_out_off_len == 0);

	/* a DS3 pad, large motor on (byte 4 only): the last motor-on payload from ANY
	 * pad replaces it */
	out_motor(&b, 48, 0x70, 0x00, 0x80);
	CHECK(shim_output_filter(&d, b.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 2);
	CHECK(d.dbg_out_on_len == 48);
	CHECK(d.dbg_out_on_pad == pad_key(&b));
	CHECK(memcmp(d.dbg_out_on_cap, b.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	CHECK(d.dbg_out_off_armed == 1);

	/* a zero-motor payload from ANOTHER pad does not touch the off capture */
	out_motor(&c, 48, 0x33, 0x00, 0x00);
	CHECK(shim_output_filter(&d, c.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 0 && d.dbg_out_off_len == 0);
	CHECK(memcmp(d.dbg_out_off_cap, zero64, 64) == 0);
	CHECK(d.dbg_out_off_armed == 1);
	CHECK(d.dbg_out_on_count == 2 && d.dbg_out_on_pad == pad_key(&b));

	/* the longest payload: 64 bytes copied, the length is 64 */
	out_motor(&a, 64, 0x01, 0xFF, 0xFF);
	CHECK(shim_output_filter(&d, a.b + SHIM_PAD_OUT_BUF_OFF, 64, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 3 && d.dbg_out_on_len == 64);
	CHECK(d.dbg_out_on_pad == pad_key(&a));
	CHECK(memcmp(d.dbg_out_on_cap, a.b + SHIM_PAD_OUT_BUF_OFF, 64) == 0);

	/* a rejected call (65) and a disabled shim capture nothing */
	CHECK(shim_output_filter(&d, a.b + SHIM_PAD_OUT_BUF_OFF, 65, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 3);
	d.enabled = 0;
	CHECK(shim_output_filter(&d, a.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 3);
	d.enabled = 1;
	CHECK(shim_output_filter(&d, NULL, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 3);

	/* observe only: no output parameter written, the payload not touched */
	CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&a && ol == OLEN_SENTINEL);
	CHECK(a.b[SHIM_PAD_OUT_BUF_OFF + 2] == 0xFF);
	return 0;
}

static int test_output_motor_off_after_on_same_pad(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t a, b;
	uint8_t rid = 0;
	const uint8_t *ob = NULL;
	uint16_t ol = 0;
	const uint8_t *abuf, *bbuf;

	pad_make(&a, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	pad_make(&b, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	abuf = a.b + SHIM_PAD_OUT_BUF_OFF;
	bbuf = b.b + SHIM_PAD_OUT_BUF_OFF;

	/* a zero-motor payload before any motor-on one: not armed, not captured */
	out_motor(&a, 48, 0x20, 0, 0);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 0 && d.dbg_out_off_len == 0);

	/* on, then the first zero from the same pad is captured once */
	out_motor(&a, 48, 0x30, 0x01, 0x40);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_armed == 1);
	out_motor(&a, 40, 0x41, 0, 0);
	CHECK(shim_output_filter(&d, abuf, 40, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 1);
	CHECK(d.dbg_out_off_len == 40);
	CHECK(memcmp(d.dbg_out_off_cap, abuf, 40) == 0);
	CHECK(d.dbg_out_off_armed == 0);
	CHECK(d.dbg_out_on_count == 1);                   /* the on capture is kept */

	/* later zero payloads are not captured */
	out_motor(&a, 48, 0x55, 0, 0);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 1 && d.dbg_out_off_len == 40);
	CHECK(d.dbg_out_off_cap[0] == (uint8_t)(0x41));

	/* a new motor-on payload re-arms; a zero from ANOTHER pad is ignored while the
	 * on pad's own zero is captured */
	out_motor(&a, 48, 0x60, 0x00, 0x01);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 2 && d.dbg_out_off_armed == 1);
	out_motor(&b, 48, 0x70, 0, 0);
	CHECK(shim_output_filter(&d, bbuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 1 && d.dbg_out_off_armed == 1);
	out_motor(&a, 48, 0x80, 0, 0);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 2 && d.dbg_out_off_len == 48);
	CHECK(memcmp(d.dbg_out_off_cap, abuf, 48) == 0);
	CHECK(d.dbg_out_off_armed == 0);

	/* the on pad moves to another pad: its zero is the one captured, the old
	 * pad's is not */
	out_motor(&a, 48, 0x90, 0x01, 0x00);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	out_motor(&b, 48, 0xA0, 0x01, 0x00);
	CHECK(shim_output_filter(&d, bbuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 4 && d.dbg_out_on_pad == pad_key(&b));
	out_motor(&a, 48, 0xB0, 0, 0);
	CHECK(shim_output_filter(&d, abuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 2 && d.dbg_out_off_armed == 1);
	out_motor(&b, 48, 0xC0, 0, 0);
	CHECK(shim_output_filter(&d, bbuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 3 && d.dbg_out_off_armed == 0);
	CHECK(memcmp(d.dbg_out_off_cap, bbuf, 48) == 0);

	/* a motor-on payload while armed keeps it armed and just replaces the capture */
	out_motor(&b, 48, 0xD0, 0x01, 0x00);
	CHECK(shim_output_filter(&d, bbuf, 48, &rid, &ob, &ol) == 0);
	out_motor(&b, 48, 0xE0, 0x00, 0xFF);
	CHECK(shim_output_filter(&d, bbuf, 48, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 6 && d.dbg_out_off_armed == 1);
	CHECK(memcmp(d.dbg_out_on_cap, bbuf, 48) == 0);

	/* the calls still return 0 and wrote no out-parameter */
	CHECK(rid == 0 && ob == NULL && ol == 0);
	return 0;
}

static int test_output_motor_short_payload(void)
{
	ds45_shim_data_t d = data_enabled();
	fake_pad_t a;
	uint8_t rid = 0;
	const uint8_t *ob = NULL;
	uint16_t ol = 0;
	const uint8_t *buf;
	int n;

	pad_make(&a, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	buf = a.b + SHIM_PAD_OUT_BUF_OFF;

	/* lengths 1..4 hold no motor bytes (bytes 2 and 4): never on, even with every
	 * byte set */
	memset(a.b + SHIM_PAD_OUT_BUF_OFF, 0xFF, 8);
	for (n = 1; n <= 4; n++)
		CHECK(shim_output_filter(&d, buf, (uint16_t)n, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 0 && d.dbg_out_on_len == 0 && d.dbg_out_on_pad == 0);
	CHECK(d.dbg_out_off_armed == 0);

	/* length 5 is the shortest that counts (byte 4 is inside it) */
	CHECK(shim_output_filter(&d, buf, 5, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_on_count == 1 && d.dbg_out_on_len == 5);
	CHECK(d.dbg_out_on_pad == pad_key(&a));
	CHECK(d.dbg_out_off_armed == 1);

	/* a short all-zero payload from the same pad is not an off */
	memset(a.b + SHIM_PAD_OUT_BUF_OFF, 0, 8);
	for (n = 1; n <= 4; n++)
		CHECK(shim_output_filter(&d, buf, (uint16_t)n, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 0 && d.dbg_out_off_len == 0);
	CHECK(d.dbg_out_off_armed == 1);

	/* length 5 and zero motors is */
	CHECK(shim_output_filter(&d, buf, 5, &rid, &ob, &ol) == 0);
	CHECK(d.dbg_out_off_count == 1 && d.dbg_out_off_len == 5);
	CHECK(d.dbg_out_off_armed == 0);

	/* short payloads did not disturb the on capture */
	CHECK(d.dbg_out_on_count == 1 && d.dbg_out_on_len == 5);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* DS3-route output hook, rewriting modes (1 probe, 2 full)                   */
/* ------------------------------------------------------------------------ */

/* A real DS3 output payload (48 bytes) at the pad's output buffer: durations
 * 0x96, the two motors, the LED bitmap, four non-zero LED timing blocks (they
 * are ignored), the rest zero. */
static void out_ds3_payload(fake_pad_t *pad, uint8_t small, uint8_t large, uint8_t leds)
{
	uint8_t *p = pad->b + SHIM_PAD_OUT_BUF_OFF;
	int i;

	memset(p, 0, 48);
	p[1] = 0x96;
	p[2] = small;
	p[3] = 0x96;
	p[4] = large;
	p[5] = 0x01;
	p[9] = leds;
	for (i = 0; i < 4; i++) {
		p[10 + 5 * i + 0] = 0xFF;
		p[10 + 5 * i + 1] = 0x27;
		p[10 + 5 * i + 2] = 0x10;
		p[10 + 5 * i + 3] = 0x00;
		p[10 + 5 * i + 4] = 0x32;
	}
}

/* The test's own reflected CRC-32 (0xEDB88320) of the byte 0xA2 followed by
 * report bytes 0..73, bit by bit. */
static uint32_t ref_crc_a2(const uint8_t *r)
{
	uint32_t crc = 0xFFFFFFFFu;
	int i, b;

	for (i = -1; i < 74; i++) {
		crc ^= (i < 0) ? 0xA2u : r[i];
		for (b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
	}
	return ~crc;
}

static int crc_bytes_ok(const uint8_t *r)
{
	uint32_t c = ref_crc_a2(r);

	return r[74] == (uint8_t)c && r[75] == (uint8_t)(c >> 8) &&
	       r[76] == (uint8_t)(c >> 16) && r[77] == (uint8_t)(c >> 24);
}

/* The data block for a rewriting mode, with the report buffer pre-filled so an
 * untouched buffer is visible. */
static ds45_shim_data_t data_out_mode(uint32_t mode, uint32_t seq)
{
	ds45_shim_data_t d = data_enabled();

	d.out_mode = mode;
	d.out_seq = seq;
	memset(d.out_buf, 0xEE, sizeof(d.out_buf));
	return d;
}

static int test_output_probe_rewrites(void)
{
	ds45_shim_data_t d = data_out_mode(1, 3);
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL, exp[DS_OUT_LEN];
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;

	/* any payload at all: the probe does not read it */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	out_payload(&pad, 48, 0x10);
	ds3_output_probe(3, exp);
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 1);
	CHECK(rid == 0x31);
	CHECK(ob == d.out_buf + 1);
	CHECK(ol == 77);
	CHECK(memcmp(d.out_buf, exp, DS_OUT_LEN) == 0);
	CHECK(d.out_buf[0] == 0x31 && d.out_buf[1] == 0x30);     /* sequence 3 */
	CHECK(crc_bytes_ok(d.out_buf));
	CHECK(d.out_buf[78] == 0xEE && d.out_buf[79] == 0xEE);   /* nothing past the report */
	CHECK(d.out_seq == 4);
	CHECK(d.dbg_out_translated == 1);
	CHECK(d.dbg_out_passed == 0);
	CHECK(d.dbg_out_calls == 1 && d.dbg_out_ds == 1);
	CHECK(d.dbg_out_cap_len == 48);                          /* the capture still runs */
	CHECK(memcmp(d.dbg_out_cap, pad.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	CHECK(pad.b[SHIM_PAD_OUT_BUF_OFF + 5] == (uint8_t)(0x10 + 15));   /* payload intact */

	/* the probe does not care about the shape: a short payload is rewritten too */
	rid = RID_SENTINEL;
	ob = (const uint8_t *)&pad;
	ol = OLEN_SENTINEL;
	ds3_output_probe(4, exp);
	CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 7, &rid, &ob, &ol) == 1);
	CHECK(rid == 0x31 && ob == d.out_buf + 1 && ol == 77);
	CHECK(memcmp(d.out_buf, exp, DS_OUT_LEN) == 0);
	CHECK(d.out_seq == 5 && d.dbg_out_translated == 2 && d.dbg_out_passed == 0);
	return 0;
}

static int test_output_full_rewrites_exact_bytes(void)
{
	ds45_shim_data_t d = data_out_mode(2, 5);
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL, exp[DS_OUT_LEN];
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;
	const uint8_t *pl = pad.b + SHIM_PAD_OUT_BUF_OFF;
	int i;

	/* player 2, the small motor on, the large motor off: the console's payload
	 * 00 96 01 96 00 00 00 00 00 04 | timing blocks | zeros */
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	memset(pad.b + SHIM_PAD_OUT_BUF_OFF, 0, 48);
	{
		static const uint8_t head[10] = { 0x00, 0x96, 0x01, 0x96, 0x00,
		                                  0x00, 0x00, 0x00, 0x00, 0x04 };
		memcpy(pad.b + SHIM_PAD_OUT_BUF_OFF, head, sizeof(head));
	}
	for (i = 10; i < 30; i++)
		pad.b[SHIM_PAD_OUT_BUF_OFF + i] = (uint8_t)(0xC0 + i);

	/* the report built by hand: id, seq 5 in the high nibble, tag, flags, right
	 * motor full, left motor off, player LEDs 0x0A, light bar red, zeros, CRC */
	memset(exp, 0, sizeof(exp));
	exp[0] = 0x31; exp[1] = 0x50; exp[2] = 0x10; exp[3] = 0x01; exp[4] = 0x14;
	exp[5] = 0xFF; exp[6] = 0x00;
	exp[46] = 0x0A;
	exp[47] = 0xFF; exp[48] = 0x00; exp[49] = 0x00;
	exp[74] = 0x21; exp[75] = 0xA8; exp[76] = 0xEC; exp[77] = 0x9F;   /* hard-coded CRC */

	CHECK(shim_output_filter(&d, pl, 48, &rid, &ob, &ol) == 1);
	CHECK(rid == 0x31 && ob == d.out_buf + 1 && ol == 77);
	CHECK(memcmp(d.out_buf, exp, DS_OUT_LEN) == 0);
	CHECK(crc_bytes_ok(d.out_buf));                         /* and the test's own CRC agrees */
	CHECK(d.out_buf[78] == 0xEE && d.out_buf[79] == 0xEE);
	CHECK(d.out_seq == 6);
	CHECK(d.dbg_out_translated == 1 && d.dbg_out_passed == 0);
	CHECK(d.dbg_out_calls == 1 && d.dbg_out_ds == 1);
	CHECK(memcmp(d.dbg_out_cap, pl, 48) == 0);

	/* both motors, player 1 (bitmap 0x02, extra non-LED bits ignored), sequence
	 * 6 now: the large motor is passed through as it is */
	out_ds3_payload(&pad, 0x00, 0x80, 0x02 | 0x01 | 0x80);
	CHECK(shim_output_filter(&d, pl, 48, &rid, &ob, &ol) == 1);
	CHECK(d.out_buf[1] == 0x60);
	CHECK(d.out_buf[5] == 0x00 && d.out_buf[6] == 0x80);
	CHECK(d.out_buf[46] == 0x04);
	CHECK(d.out_buf[47] == 0x00 && d.out_buf[48] == 0x00 && d.out_buf[49] == 0xFF);
	CHECK(crc_bytes_ok(d.out_buf));
	CHECK(d.out_seq == 7 && d.dbg_out_translated == 2 && d.dbg_out_passed == 0);

	/* no LED bit: LEDs and light bar off */
	out_ds3_payload(&pad, 0x00, 0x00, 0x00);
	CHECK(shim_output_filter(&d, pl, 48, &rid, &ob, &ol) == 1);
	CHECK(d.out_buf[46] == 0 && d.out_buf[47] == 0 && d.out_buf[48] == 0 && d.out_buf[49] == 0);
	CHECK(d.out_buf[5] == 0 && d.out_buf[6] == 0);
	CHECK(crc_bytes_ok(d.out_buf));
	return 0;
}

static int test_output_two_pads_use_own_leds(void)
{
	ds45_shim_data_t d = data_out_mode(2, 0);
	fake_pad_t a, b;
	uint8_t rid = 0, saved[DS_OUT_LEN];
	const uint8_t *ob = NULL;
	uint16_t ol = 0;
	int i;

	pad_make(&a, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	pad_make(&b, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	out_ds3_payload(&a, 0x00, 0x00, 0x02);   /* player 1: blue */
	out_ds3_payload(&b, 0x01, 0x40, 0x04);   /* player 2: red  */

	/* alternate the two pads: one counter 0,1,2,... wrapping 15 -> 0, each report
	 * carrying its own pad's LEDs and colour */
	for (i = 0; i < 20; i++) {
		fake_pad_t *p = (i & 1) ? &b : &a;

		rid = 0; ob = NULL; ol = 0;
		CHECK(shim_output_filter(&d, p->b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 1);
		CHECK(rid == 0x31 && ob == d.out_buf + 1 && ol == 77);
		memcpy(saved, d.out_buf, sizeof(saved));
		CHECK(saved[0] == 0x31);
		CHECK((saved[1] >> 4) == (i & 0x0F));            /* 0..15, then back to 0 */
		CHECK((saved[1] & 0x0F) == 0);
		CHECK(saved[2] == 0x10 && saved[3] == 0x01 && saved[4] == 0x14);
		if (i & 1) {
			CHECK(saved[5] == 0xFF && saved[6] == 0x40);
			CHECK(saved[46] == 0x0A);
			CHECK(saved[47] == 0xFF && saved[48] == 0x00 && saved[49] == 0x00);
		} else {
			CHECK(saved[5] == 0x00 && saved[6] == 0x00);
			CHECK(saved[46] == 0x04);
			CHECK(saved[47] == 0x00 && saved[48] == 0x00 && saved[49] == 0xFF);
		}
		CHECK(crc_bytes_ok(saved));
		CHECK(d.out_seq == (uint32_t)((i + 1) & 0x0F));
	}
	CHECK(d.dbg_out_translated == 20 && d.dbg_out_passed == 0);
	CHECK(d.dbg_out_calls == 20 && d.dbg_out_ds == 20);
	return 0;
}

static int test_output_malformed_dualsense_passes(void)
{
	ds45_shim_data_t d = data_out_mode(2, 7);
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = (const uint8_t *)&pad;
	uint16_t ol = OLEN_SENTINEL;
	const uint8_t *pl = pad.b + SHIM_PAD_OUT_BUF_OFF;
	uint16_t lens[] = { 1, 5, 47, 49, 64 };
	unsigned i, calls = 0;
	static const int bad[] = { 7, 8, 30, 47 };

	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);

	/* wrong length */
	out_ds3_payload(&pad, 0x01, 0x40, 0x02);
	for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
		CHECK(shim_output_filter(&d, pl, lens[i], &rid, &ob, &ol) == 0);
		calls++;
	}
	/* right length, wrong shape: a byte that must be zero is not */
	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		out_ds3_payload(&pad, 0x01, 0x40, 0x02);
		pad.b[SHIM_PAD_OUT_BUF_OFF + bad[i]] = 0x01;
		CHECK(shim_output_filter(&d, pl, 48, &rid, &ob, &ol) == 0);
		calls++;
	}

	CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
	CHECK(d.out_seq == 7);
	{
		uint8_t ee[sizeof(d.out_buf)];

		memset(ee, 0xEE, sizeof(ee));
		CHECK(memcmp(d.out_buf, ee, sizeof(ee)) == 0);
	}
	CHECK(d.dbg_out_translated == 0);
	CHECK(d.dbg_out_passed == calls);
	CHECK(d.dbg_out_calls == calls && d.dbg_out_ds == calls);

	/* a good payload right after still goes through, with the unadvanced counter */
	out_ds3_payload(&pad, 0x01, 0x40, 0x02);
	CHECK(shim_output_filter(&d, pl, 48, &rid, &ob, &ol) == 1);
	CHECK(d.out_buf[1] == 0x70 && d.out_seq == 8);
	return 0;
}

static int test_output_ds3_never_rewritten_all_modes(void)
{
	uint32_t mode;
	int kind;

	/* kind 0: a DualShock 3 (the Sony pad class, PID 0x0268); 1: a pad of another
	 * class; 2: a Sony class pad with another PID; 3: a pad with a wrong vendor */
	for (kind = 0; kind < 4; kind++) {
		for (mode = 0; mode <= 3; mode++) {
			ds45_shim_data_t d = data_out_mode(mode, 9), before;
			fake_pad_t pad, pad_before;
			uint8_t rid = RID_SENTINEL;
			const uint8_t *ob = (const uint8_t *)&pad;
			uint16_t ol = OLEN_SENTINEL;

			if (kind == 0)
				pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DS3);
			else if (kind == 1)
				pad_make(&pad, PAD_VTABLE + 8, VID_SONY, PID_DUALSENSE);
			else if (kind == 2)
				pad_make(&pad, PAD_VTABLE, VID_SONY, 0x042F);
			else
				pad_make(&pad, PAD_VTABLE, 0x1234, PID_DUALSENSE);
			out_ds3_payload(&pad, 0x01, 0x40, 0x04);
			pad_before = pad;
			before = d;
			CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
			CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
			CHECK(memcmp(pad.b, pad_before.b, sizeof(pad.b)) == 0);
			CHECK(d.out_seq == 9 && d.dbg_out_translated == 0);
			CHECK(memcmp(d.out_buf, before.out_buf, sizeof(d.out_buf)) == 0);
			CHECK(d.dbg_out_calls == 1 && d.dbg_out_ds == 0 && d.dbg_out_passed == 1);
			CHECK(d.dbg_out_cap_len == 48);
			CHECK(memcmp(d.dbg_out_cap, pad.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
		}
	}
	return 0;
}

static int test_output_mode0_never_rewrites(void)
{
	static const uint32_t modes[] = { 0, 3, 4, 7, 16, 255, 0x80000000u, 0xFFFFFFFFu };
	unsigned i;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		ds45_shim_data_t d = data_out_mode(modes[i], 2), before;
		fake_pad_t pad, pad_before;
		uint8_t rid = RID_SENTINEL;
		const uint8_t *ob = (const uint8_t *)&pad;
		uint16_t ol = OLEN_SENTINEL;

		pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
		out_ds3_payload(&pad, 0x01, 0x40, 0x04);
		pad_before = pad;
		before = d;
		CHECK(shim_output_filter(&d, pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
		CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
		CHECK(memcmp(pad.b, pad_before.b, sizeof(pad.b)) == 0);
		CHECK(d.out_seq == 2 && d.dbg_out_translated == 0);
		CHECK(memcmp(d.out_buf, before.out_buf, sizeof(d.out_buf)) == 0);
		/* observed exactly as before: a DualSense, passed */
		CHECK(d.dbg_out_calls == 1 && d.dbg_out_ds == 1 && d.dbg_out_passed == 1);
		CHECK(d.dbg_out_cap_len == 48);
	}
	return 0;
}

static int test_output_bad_arguments_in_modes(void)
{
	uint32_t mode;

	for (mode = 1; mode <= 2; mode++) {
		ds45_shim_data_t d = data_out_mode(mode, 4), zero;
		fake_pad_t pad;
		uint8_t rid = RID_SENTINEL;
		const uint8_t *ob = (const uint8_t *)&pad;
		uint16_t ol = OLEN_SENTINEL;
		const uint8_t *buf;

		pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
		out_ds3_payload(&pad, 0x01, 0x40, 0x04);
		buf = pad.b + SHIM_PAD_OUT_BUF_OFF;

		/* NULL data block */
		CHECK(shim_output_filter(NULL, buf, 48, &rid, &ob, &ol) == 0);

		/* NULL buffer, length 0, length 65: counted as calls, nothing else */
		zero = d;
		CHECK(shim_output_filter(&d, NULL, 48, &rid, &ob, &ol) == 0);
		zero.dbg_out_calls = 1;
		CHECK(memcmp(&d, &zero, sizeof(d)) == 0);
		CHECK(shim_output_filter(&d, buf, 0, &rid, &ob, &ol) == 0);
		zero.dbg_out_calls = 2;
		CHECK(memcmp(&d, &zero, sizeof(d)) == 0);
		CHECK(shim_output_filter(&d, buf, 65, &rid, &ob, &ol) == 0);
		zero.dbg_out_calls = 3;
		CHECK(memcmp(&d, &zero, sizeof(d)) == 0);

		/* shim disabled */
		d = data_out_mode(mode, 4);
		d.enabled = 0;
		zero = d;
		CHECK(shim_output_filter(&d, buf, 48, &rid, &ob, &ol) == 0);
		zero.dbg_out_calls = 1;
		CHECK(memcmp(&d, &zero, sizeof(d)) == 0);

		CHECK(rid == RID_SENTINEL && ob == (const uint8_t *)&pad && ol == OLEN_SENTINEL);
		CHECK(d.out_seq == 4 && d.dbg_out_translated == 0);
	}
	return 0;
}

/* The glue: on the host shim_output is the C body itself (entry.c), and it
 * decides on the one shared data object. */
static int test_glue_output(void)
{
	fake_pad_t pad;
	uint8_t rid = RID_SENTINEL;
	const uint8_t *ob = NULL;
	uint16_t ol = OLEN_SENTINEL;

	ds45_shim_data = data_enabled();
	pad_make(&pad, PAD_VTABLE, VID_SONY, PID_DUALSENSE);
	out_payload(&pad, 48, 0x20);
	CHECK(shim_output(pad.b + SHIM_PAD_OUT_BUF_OFF, 48, &rid, &ob, &ol) == 0);
	CHECK(ds45_shim_data.dbg_out_calls == 1);
	CHECK(ds45_shim_data.dbg_out_ds == 1);
	CHECK(ds45_shim_data.dbg_out_passed == 1);
	CHECK(ds45_shim_data.dbg_out_cap_len == 48);
	CHECK(memcmp(ds45_shim_data.dbg_out_cap, pad.b + SHIM_PAD_OUT_BUF_OFF, 48) == 0);
	CHECK(rid == RID_SENTINEL && ob == NULL && ol == OLEN_SENTINEL);
	return 0;
}

int run_tests(void)
{
	int (*const tests[])(void) = {
		test_output_counts_and_captures,
		test_output_ds3_pad_untouched,
		test_output_bad_arguments,
		test_output_capture_keeps_last,
		test_output_motor_on_capture,
		test_output_motor_off_after_on_same_pad,
		test_output_motor_short_payload,
		test_output_probe_rewrites,
		test_output_full_rewrites_exact_bytes,
		test_output_two_pads_use_own_leds,
		test_output_malformed_dualsense_passes,
		test_output_ds3_never_rewritten_all_modes,
		test_output_mode0_never_rewrites,
		test_output_bad_arguments_in_modes,
		test_glue_output,
		test_stable_identity_from_connection,
		test_stable_identity_survives_reconnect,
		test_stable_identity_fallbacks,
		test_stable_identity_off,
		test_pid_ok_table,
		test_pad_identity,
		test_event_dualsense_completes,
		test_event_identities_differ,
		test_event_after_done_passes,
		test_event_ds3_untouched,
		test_event_bad_pad_untouched,
		test_event_disabled,
		test_event_info_not_pad_untouched,
		test_enable_dualsense_completes,
		test_enable_passes,
		test_pad_from_xport,
		test_input_ds3_untouched,
		test_input_dualsense_translated,
		test_input_short_passthrough,
		test_input_disabled_passthrough,
		test_input_identity_unknown_passthrough,
		test_glue_event,
		test_glue_enable,
		test_input_basic_translated,
		test_input_passthrough,
		test_dbg_counts_event,
		test_dbg_counts_enable,
		test_input_note,
		test_input_mode0_unchanged,
		test_input_ds3_bounced_unchanged,
		test_input_bounce_translates_full_report,
		test_input_bounce_basic_report,
		test_input_bounce_never_overruns_dst,
		test_input_bounce_error_and_zero,
		test_input_bounce_drop_oversize,
		test_input_bounce_counters,
		test_input_bounce_counters_other_pad,
		test_input_bounce_pad_lookup,
		test_input_raw_capture_full_report,
		test_input_raw_capture_basic_report,
		test_input_raw_capture_only_dualsense,
		test_input_raw_capture_seq_counts,
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
