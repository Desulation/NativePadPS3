/* SOURCES: ds_body.c sixaxis.c ../shim/ds3_report.c ../shim/shim.c ../shim/entry.c */
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
#include "../shim/shim.h"

/* shim/entry.c: the one data object the plugin and the blob share, and the
 * three wrappers the kernel's redirects point at. */
extern ds45_shim_data_t ds45_shim_data;
int shim_event(void *drv, long status, long len);
int shim_enable(void *drv, long status, long len);
int shim_input(void *xport, uint8_t *dst, uint16_t len_in, int flag);

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
static uint8_t  g_oi_report[RPT_LEN];  /* what the original read delivers ... */
static int      g_oi_len;              /* ... and how many bytes it reports */

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
	(void)xport;
	(void)len_in;
	(void)flag;
	g_oi_calls++;
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

int run_tests(void)
{
	int (*const tests[])(void) = {
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
