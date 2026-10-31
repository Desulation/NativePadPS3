/* SOURCES: ds_body.c sixaxis.c ../shim/ds3_report.c ../shim/shim.c */
/*
 * Host tests for the kernel shim's decision logic (shim/shim.c): the F4
 * product-ID helper, the F2 input filter and the F1 DualSense start-up. The
 * functions take the data block and the already-extracted arguments
 * explicitly, so they are pure and testable with no kernel calls; shim/entry.c
 * (the kernel-ABI glue that extracts those arguments) is not exercised here.
 *
 * The synthetic 0x31 report is built from the DualSense Bluetooth offsets the
 * same way tests/test_ds3report.c builds it.
 */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "ds45_state.h"
#include "../shim/shim_abi.h"
#include "../shim/ds3_report.h"
#include "../shim/shim.h"

/* Sony USB identity. */
#define VID_SONY        0x054C
#define PID_DS3         0x0268
#define PID_MOVE_NAV    0x042F
#define PID_DS4         0x05C4
#define PID_DUALSENSE   0x0CE6

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

/* A zeroed, enabled data block with a plain config (magic/version pre-filled,
 * as the blob ships them). */
static ds45_shim_data_t data_enabled(void)
{
	ds45_shim_data_t d;

	memset(&d, 0, sizeof(d));
	d.magic = DS45_SHIM_MAGIC;
	d.version = DS45_SHIM_VERSION;
	d.enabled = 1;
	d.cfg = cfg_plain();
	return d;
}

/* ------------------------------------------------------------------------ */
/* A fake get_report (F3): it records what GET was asked for and returns a
 * settable result, standing in for the plugin's real BT-HID sender.          */
/* ------------------------------------------------------------------------ */

static int     g_gr_calls;
static void   *g_gr_ctx;
static uint8_t g_gr_type;
static uint8_t g_gr_id;
static int     g_gr_rc;

static int fake_get_report(void *ctx, uint8_t report_type, uint8_t report_id)
{
	g_gr_calls++;
	g_gr_ctx  = ctx;
	g_gr_type = report_type;
	g_gr_id   = report_id;
	return g_gr_rc;
}

static void gr_reset(int rc)
{
	g_gr_calls = 0;
	g_gr_ctx   = 0;
	g_gr_type  = 0xFF;
	g_gr_id    = 0xFF;
	g_gr_rc    = rc;
}

/* ------------------------------------------------------------------------ */
/* F4: product-ID acceptance                                                  */
/* ------------------------------------------------------------------------ */

static int test_pid_ok_table(void)
{
	/* accepted: Sony VID with the DS3, Move-navigation or DualSense PID */
	CHECK(shim_pid_ok(VID_SONY, PID_DS3) == 1);
	CHECK(shim_pid_ok(VID_SONY, PID_MOVE_NAV) == 1);
	CHECK(shim_pid_ok(VID_SONY, PID_DUALSENSE) == 1);

	/* rejected: a DS4, the wrong VID, the inline-site-only 0x03D5, and zeros */
	CHECK(shim_pid_ok(VID_SONY, PID_DS4) == 0);
	CHECK(shim_pid_ok(0x054D, PID_DUALSENSE) == 0);
	CHECK(shim_pid_ok(VID_SONY, 0x03D5) == 0);
	CHECK(shim_pid_ok(VID_SONY, 0x0000) == 0);
	CHECK(shim_pid_ok(0x0000, PID_DS3) == 0);
	CHECK(shim_pid_ok(0x046D, 0xC260) == 0);

	/* shim_is_dualsense is the narrower gate the input/start-up paths key on */
	CHECK(shim_is_dualsense(VID_SONY, PID_DUALSENSE) == 1);
	CHECK(shim_is_dualsense(VID_SONY, PID_DS3) == 0);
	CHECK(shim_is_dualsense(VID_SONY, PID_DS4) == 0);
	CHECK(shim_is_dualsense(0x054D, PID_DUALSENSE) == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* F2: input filter                                                           */
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

/* The F2 identity lookup failing (entry.c passes an unknown VID/PID) leaves the
 * buffer untouched and returns the original result. */
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
/* F1: DualSense start-up                                                     */
/* ------------------------------------------------------------------------ */

/* Start-up issues exactly the feature-0x05 GET and returns F1's running
 * condition. (That this GET actually flips the hardware to 0x31 is the
 * unverified U2, tested on-console in a later task - not here.) */
static int test_startup_issues_feature_05(void)
{
	ds45_shim_data_t d = data_enabled();
	int drv = 0;                   /* a stand-in driver object */
	int rc;

	d.get_report = (uint64_t)(uintptr_t)fake_get_report;
	gr_reset(SHIM_F1_OK);          /* the GET succeeds */

	rc = shim_startup_dualsense(&d, &drv, 1 /* arg1 != 0 */);

	CHECK(g_gr_calls == 1);                     /* exactly one GET issued */
	CHECK(g_gr_type == SHIM_HID_FEATURE);       /* a feature report (0x03) */
	CHECK(g_gr_id == SHIM_DS_CALIB_REPORT_ID);  /* report id 0x05 */
	CHECK(g_gr_ctx == (void *)&drv);            /* on this connection */
	CHECK(rc == SHIM_F1_OK);                    /* F1's running condition */
	CHECK(d.stat_startups == 1);
	return 0;
}

/* When the GET fails the start-up propagates the error, bumps no counter and
 * does not crash (F1's negative-error path). */
static int test_startup_get_report_fails(void)
{
	ds45_shim_data_t d = data_enabled();
	int drv = 0;
	int rc;

	d.get_report = (uint64_t)(uintptr_t)fake_get_report;
	gr_reset(SHIM_F1_ERR);         /* the GET fails with an LV2 error */

	rc = shim_startup_dualsense(&d, &drv, 1);

	CHECK(g_gr_calls == 1);                /* it did go through get_report */
	CHECK(rc == SHIM_F1_ERR);              /* the failure is propagated */
	CHECK(rc != SHIM_F1_OK);
	CHECK(d.stat_startups == 0);           /* counter unchanged */
	return 0;
}

int run_tests(void)
{
	int (*const tests[])(void) = {
		test_pid_ok_table,
		test_input_ds3_untouched,
		test_input_dualsense_translated,
		test_input_short_passthrough,
		test_input_disabled_passthrough,
		test_input_identity_unknown_passthrough,
		test_startup_issues_feature_05,
		test_startup_get_report_fails,
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
