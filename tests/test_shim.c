/* SOURCES: ds_body.c sixaxis.c ../shim/ds3_report.c ../shim/shim.c ../shim/entry.c */
/*
 * Host tests for the kernel shim: the decision logic in shim/shim.c (the F4
 * product-ID helper, the F2 input filter and the F1 DualSense start-up) and the
 * kernel-ABI glue in shim/entry.c that calls it. The decision functions take
 * the data block and the already-extracted arguments explicitly, so they are
 * pure and testable with no kernel calls. entry.c is linked in as well - its
 * F1/F2 wrappers read the handle out of a plain byte buffer standing in for the
 * driver object and call the original through a descriptor address, both of
 * which a host function can fake - so which connections reach the DualSense
 * path, which tail-call the original, and which connection id the start-up GET
 * is sent to are all exercised here too.
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

/* shim/entry.c: the one data object the plugin and the blob share, and the two
 * wrappers the kernel's redirects point at (the F4 helper is shim_pid_ok). */
extern ds45_shim_data_t ds45_shim_data;
int shim_startup(void *drv, long arg1);
int shim_input(void *xport, uint8_t *dst, uint16_t len_in, int flag);

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
/* A fake of the kernel's Bluetooth HID transfer routine (note P1):
 *     int h116ef0(u64 conn_id, int dir, int opclass, void *buf, int len)
 * It records every argument - and what buf held at the time of the call, since
 * the shim's buffer is reused - and returns a settable result.               */
/* ------------------------------------------------------------------------ */

static int      g_send_calls;
static uint64_t g_send_conn;
static int      g_send_dir, g_send_opclass, g_send_len;
static void    *g_send_buf;
static uint8_t  g_send_pdu[8];
static int      g_send_rc;

static int fake_send(uint64_t conn_id, int dir, int opclass, void *buf, int len)
{
	g_send_calls++;
	g_send_conn    = conn_id;
	g_send_dir     = dir;
	g_send_opclass = opclass;
	g_send_len     = len;
	g_send_buf     = buf;
	memset(g_send_pdu, 0xEE, sizeof(g_send_pdu));
	if (buf && len > 0 && len <= (int)sizeof(g_send_pdu))
		memcpy(g_send_pdu, buf, (size_t)len);
	return g_send_rc;
}

static void send_reset(int rc)
{
	g_send_calls   = 0;
	g_send_conn    = 0;
	g_send_dir     = -1;
	g_send_opclass = -1;
	g_send_len     = -1;
	g_send_buf     = 0;
	memset(g_send_pdu, 0xEE, sizeof(g_send_pdu));
	g_send_rc      = rc;
}

/* Connection ids use all 64 bits, so a narrowing or a swapped argument shows. */
#define CONN_ID_A   0x0123456789ABCDEFULL
#define CONN_ID_B   0xFEDCBA9876543210ULL
#define CONN_ID_C   0x00000000DEADBEEFULL

/* ------------------------------------------------------------------------ */
/* Fakes of the originals the glue tail-calls (F1 start-up, F2 input read).   */
/* ------------------------------------------------------------------------ */

#define ORIG_STARTUP_RC  0x1357        /* what the original start-up returns */
#define ORIG_INPUT_LEN   64            /* what the original input read returns */

static int   g_os_calls;
static void *g_os_drv;
static long  g_os_arg1;
static int   g_oi_calls;
static uint8_t g_oi_report[RPT_LEN];   /* the report the original read delivers */

static int fake_orig_startup(void *drv, long arg1)
{
	g_os_calls++;
	g_os_drv  = drv;
	g_os_arg1 = arg1;
	return ORIG_STARTUP_RC;
}

static int fake_orig_input(void *xport, uint8_t *dst, uint16_t len_in, int flag)
{
	(void)xport;
	(void)len_in;
	(void)flag;
	g_oi_calls++;
	memcpy(dst, g_oi_report, ORIG_INPUT_LEN);
	return ORIG_INPUT_LEN;
}

/* The glue works on the real, global data block: reset it to enabled, with the
 * fakes above as the originals and the send routine and no connections known. */
static void glue_reset(void)
{
	memset(&ds45_shim_data, 0, sizeof(ds45_shim_data));
	ds45_shim_data.magic        = DS45_SHIM_MAGIC;
	ds45_shim_data.version      = DS45_SHIM_VERSION;
	ds45_shim_data.enabled      = 1;
	ds45_shim_data.cfg          = cfg_plain();
	ds45_shim_data.orig_startup = (uint64_t)(uintptr_t)fake_orig_startup;
	ds45_shim_data.orig_input   = (uint64_t)(uintptr_t)fake_orig_input;
	ds45_shim_data.send_routine = (uint64_t)(uintptr_t)fake_send;
	send_reset(2);
	g_os_calls = 0;
	g_os_drv   = 0;
	g_os_arg1  = -1;
	g_oi_calls = 0;
}

/* What the plugin records when a DualSense connects: the handle the hooks see,
 * the connection id the send needs, and the slot's state last. */
static void slot_set(int i, uint8_t handle, uint64_t conn_id)
{
	ds45_shim_data.conn_key[i]   = handle;
	ds45_shim_data.conn_id[i]    = conn_id;
	ds45_shim_data.conn_state[i] = 1;
}

/* A stand-in for the F1 driver object (handle byte at drv+0xAC) and the F2
 * transport object (handle byte at xport+8). */
#define F1_HANDLE_OFF   0xAC
#define F2_HANDLE_OFF   0x08
#define OBJ_LEN         0x100

static void drv_make(uint8_t drv[OBJ_LEN], uint8_t handle)
{
	memset(drv, 0x5A, OBJ_LEN);        /* garbage around the handle byte */
	drv[F1_HANDLE_OFF] = handle;
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

	/* rejected: a DS4, the wrong VID, a Move motion controller (0x03D5), zeros */
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

/* The slot the pure start-up tests use. Not 0, so mixing it up with the first
 * slot shows; the glue tests below cover slot 0 and the last slot explicitly. */
#define PURE_SLOT  5

/* Start-up sends the feature-0x05 GET through the kernel's transfer routine
 * with the convention of note P1 - connection id in r3, control-out channel
 * dir 2, report-transfer opclass 2, the PDU {0x43, 0x05} (GET_REPORT | feature,
 * report id 5) of length 2 in the data block's OWN buffer - and returns F1's
 * running condition. It sends only: no reply drain (P2). (That this GET really
 * flips the hardware to 0x31 is the on-console R2, not tested here.) */
static int test_startup_sends_feature_05_get(void)
{
	ds45_shim_data_t d = data_enabled(), want;
	int rc;

	d.send_routine = (uint64_t)(uintptr_t)fake_send;
	d.conn_id[PURE_SLOT] = CONN_ID_A;
	send_reset(2);                 /* a successful send reports 2 bytes moved */
	memcpy(&want, &d, sizeof(d));

	rc = shim_startup_dualsense(&d, PURE_SLOT, 1 /* arg1 != 0 */);

	CHECK(g_send_calls == 1);                 /* exactly one send, no reply drain */
	CHECK(g_send_conn == CONN_ID_A);          /* r3: this connection, all 64 bits */
	CHECK(g_send_dir == 2);                   /* r4: control channel, TX */
	CHECK(g_send_opclass == 2);               /* r5: HID report transfer */
	CHECK(g_send_len == 2);                   /* r7: the PDU length only */
	CHECK(g_send_buf == (void *)d.pdu);       /* r6: blob-owned, never the driver's */
	CHECK(g_send_pdu[0] == 0x43);             /* GET_REPORT | feature */
	CHECK(g_send_pdu[1] == 0x05);             /* the calibration report id */
	CHECK(rc == SHIM_F1_OK);                  /* F1's running condition */
	CHECK(d.stat_startups == 1);
	CHECK(d.start_tries[PURE_SLOT] == 1);     /* one send spent from this slot's budget */

	/* the only data-block bytes the start-up touched are the PDU, this slot's
	 * send count and the start-up counter */
	want.pdu[0] = 0x43;
	want.pdu[1] = 0x05;
	want.start_tries[PURE_SLOT] = 1;
	want.stat_startups = 1;
	CHECK(memcmp(&want, &d, sizeof(d)) == 0);
	return 0;
}

/* Each slot sends to its own connection id - the first and last slots included
 * - and spends only its own budget. */
static int test_startup_uses_the_slots_own_conn_id(void)
{
	int s, k;

	for (s = 0; s < DS45_SHIM_CONNS; s++) {
		ds45_shim_data_t d = data_enabled();

		d.send_routine = (uint64_t)(uintptr_t)fake_send;
		for (k = 0; k < DS45_SHIM_CONNS; k++)
			d.conn_id[k] = CONN_ID_A + (uint64_t)k;      /* distinct, none 0 */
		send_reset(2);

		CHECK(shim_startup_dualsense(&d, s, 1) == SHIM_F1_OK);
		CHECK(g_send_calls == 1);
		CHECK(g_send_conn == CONN_ID_A + (uint64_t)s);
		for (k = 0; k < DS45_SHIM_CONNS; k++)
			CHECK(d.start_tries[k] == (k == s ? 1u : 0u));
	}
	return 0;
}

/* The send reports bytes moved on success (>= 0) and a negative 0x8001xxxx LV2
 * error otherwise (note P1); every non-negative result is the running condition. */
static int test_startup_send_success_codes(void)
{
	static const int ok[] = { 0, 2, 4 };
	unsigned i;

	for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		ds45_shim_data_t d = data_enabled();

		d.send_routine = (uint64_t)(uintptr_t)fake_send;
		d.conn_id[2] = CONN_ID_B;
		send_reset(ok[i]);

		CHECK(shim_startup_dualsense(&d, 2, 1) == SHIM_F1_OK);
		CHECK(g_send_calls == 1);
		CHECK(d.stat_startups == 1);
	}
	return 0;
}

/* When the send fails the start-up propagates that error unchanged, counts
 * nothing and does not retry (F1's negative-error path). */
static int test_startup_send_fails(void)
{
	static const int err[] = {
		(int)0x80010002U,          /* bad dir/len */
		(int)0x8001000DU,          /* transfer error */
		(int)0x80010028U,          /* transfer error */
		(int)0x8001002DU,          /* wrong channel mode */
	};
	unsigned i;

	for (i = 0; i < sizeof(err) / sizeof(err[0]); i++) {
		ds45_shim_data_t d = data_enabled();
		int rc;

		d.send_routine = (uint64_t)(uintptr_t)fake_send;
		d.conn_id[2] = CONN_ID_A;
		send_reset(err[i]);

		rc = shim_startup_dualsense(&d, 2, 1);

		CHECK(g_send_calls == 1);          /* it did try, once */
		CHECK(rc == err[i]);               /* the failure is propagated as is */
		CHECK(rc < 0);
		CHECK(d.stat_startups == 0);       /* counter unchanged */
	}
	return 0;
}

/* A data block with no send routine (a table row without one) cannot enable the
 * pad: the start-up fails safe, sends nothing and does not crash. */
static int test_startup_without_send_routine(void)
{
	ds45_shim_data_t d = data_enabled();
	int rc;

	d.conn_id[1] = CONN_ID_A;
	send_reset(2);                 /* d.send_routine stays 0 */

	rc = shim_startup_dualsense(&d, 1, 1);

	CHECK(rc == SHIM_F1_ERR);
	CHECK(g_send_calls == 0);
	CHECK(d.stat_startups == 0);
	CHECK(d.start_tries[1] == 0);  /* a send that was never possible spends no budget */
	CHECK(d.pdu[0] == 0 && d.pdu[1] == 0);
	return 0;
}

/* A connection whose id was never captured (0) is not sent to: the kernel's
 * routine would look a bogus id up. The start-up fails safe instead. */
static int test_startup_without_conn_id(void)
{
	ds45_shim_data_t d = data_enabled();
	int rc;

	d.send_routine = (uint64_t)(uintptr_t)fake_send;
	d.conn_id[3] = 0;
	d.conn_id[4] = CONN_ID_B;      /* a neighbour's id must not be borrowed */
	send_reset(2);

	rc = shim_startup_dualsense(&d, 3, 1);

	CHECK(rc == SHIM_F1_ERR);
	CHECK(g_send_calls == 0);
	CHECK(d.stat_startups == 0);
	CHECK(d.start_tries[3] == 0);
	return 0;
}

/* A slot outside the table (the glue never produces one) is refused and nothing
 * is sent. The words either side of conn_id[] are made non-zero, so a read that
 * was not bounds-checked would "find" an id and send. */
static int test_startup_bad_slot_fails_safe(void)
{
	static const int bad[] = { -1, DS45_SHIM_CONNS, DS45_SHIM_CONNS + 1 };
	unsigned i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		ds45_shim_data_t d = data_enabled();
		int k;

		d.send_routine = (uint64_t)(uintptr_t)fake_send;
		d.send_opd[2] = 0x1111111111111111ULL;       /* just before conn_id[] */
		memset(d.pdu, 0xFF, sizeof(d.pdu));          /* just after it */
		for (k = 0; k < DS45_SHIM_CONNS; k++)
			d.conn_id[k] = CONN_ID_A;
		send_reset(2);

		CHECK(shim_startup_dualsense(&d, bad[i], 1) == SHIM_F1_ERR);
		CHECK(g_send_calls == 0);
		CHECK(d.stat_startups == 0);
	}
	return 0;
}

/* The driver may call the start-up again and again for one connection: the
 * original steps a persisted substate machine and the shim never advances it
 * (fact sheet F1). Each call that sends re-sends the GET and, if the pad never
 * switches, leaves a reply nobody drains - so the shim gives each connection a
 * budget of SHIM_START_TRIES_MAX sends. After that the hook still answers F1's
 * OK (an error could make the driver drop a pad that did switch), sends
 * nothing, and the budget is the slot's own. Zeroing the slot's count - what
 * kshim_set_connection does before it publishes a connection - re-arms it. */
static int test_startup_send_is_capped_per_slot(void)
{
	ds45_shim_data_t d = data_enabled();
	int i;

	CHECK(SHIM_START_TRIES_MAX == 8);
	d.send_routine = (uint64_t)(uintptr_t)fake_send;
	d.conn_id[0] = CONN_ID_A;
	d.conn_id[4] = CONN_ID_B;
	send_reset(2);

	for (i = 1; i <= SHIM_START_TRIES_MAX; i++) {         /* the budget is spent ... */
		CHECK(shim_startup_dualsense(&d, 0, 1) == SHIM_F1_OK);
		CHECK(g_send_calls == i);
		CHECK(d.start_tries[0] == (uint64_t)i);
	}
	for (i = 0; i < 20; i++)                              /* ... and then it is not */
		CHECK(shim_startup_dualsense(&d, 0, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX);
	CHECK(d.start_tries[0] == SHIM_START_TRIES_MAX);      /* no further bumps */
	CHECK(d.stat_startups == SHIM_START_TRIES_MAX);

	/* the budget is per slot: another connection still sends, to its own id */
	CHECK(shim_startup_dualsense(&d, 4, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX + 1);
	CHECK(g_send_conn == CONN_ID_B);
	CHECK(d.start_tries[4] == 1);

	/* re-armed (the plugin zeroes the slot's count), the first slot sends again */
	d.start_tries[0] = 0;
	CHECK(shim_startup_dualsense(&d, 0, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX + 2);
	CHECK(g_send_conn == CONN_ID_A);
	CHECK(d.start_tries[0] == 1);
	return 0;
}

/* Every send that is made spends budget, whether or not the kernel accepted it:
 * a connection the transfer routine keeps refusing is not retried without end
 * either. At the cap the answer is F1's OK, as after a successful budget (see
 * above): the pad is simply left alone, as it would be with no shim. */
static int test_startup_failed_sends_spend_the_budget(void)
{
	ds45_shim_data_t d = data_enabled();
	int i;

	d.send_routine = (uint64_t)(uintptr_t)fake_send;
	d.conn_id[3] = CONN_ID_A;
	send_reset((int)0x8001002DU);

	for (i = 1; i <= SHIM_START_TRIES_MAX; i++) {
		CHECK(shim_startup_dualsense(&d, 3, 1) == (int)0x8001002DU);
		CHECK(g_send_calls == i);
		CHECK(d.start_tries[3] == (uint64_t)i);
	}
	CHECK(shim_startup_dualsense(&d, 3, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX);          /* the kernel is not called again */
	CHECK(d.stat_startups == 0);                          /* none of them succeeded */
	return 0;
}

/* ------------------------------------------------------------------------ */
/* F1 glue (entry.c): which connections get the GET, and to which id          */
/* ------------------------------------------------------------------------ */

/* An enabled DualSense connection's start-up is sent to THAT connection's id,
 * taken from the slot the plugin filled for the handle at drv+0xAC - not the
 * first slot, not another DualSense's - and never reaches the original. */
static int test_glue_startup_sends_to_the_slots_conn_id(void)
{
	uint8_t drv[OBJ_LEN];
	int rc;

	glue_reset();
	slot_set(0, 0x11, CONN_ID_C);
	slot_set(3, 0x2A, CONN_ID_A);
	slot_set(5, 0x2B, CONN_ID_B);
	drv_make(drv, 0x2A);

	rc = shim_startup(drv, 1);

	CHECK(rc == SHIM_F1_OK);
	CHECK(g_send_calls == 1);
	CHECK(g_send_conn == CONN_ID_A);               /* slot 3's, by handle 0x2A */
	CHECK(g_send_dir == 2);
	CHECK(g_send_opclass == 2);
	CHECK(g_send_len == 2);
	CHECK(g_send_buf == (void *)ds45_shim_data.pdu);
	CHECK(g_send_pdu[0] == 0x43 && g_send_pdu[1] == 0x05);
	CHECK(g_os_calls == 0);                        /* the original never ran */
	CHECK(ds45_shim_data.stat_startups == 1);

	/* a second DualSense, another handle: its own id */
	send_reset(2);
	drv_make(drv, 0x2B);
	CHECK(shim_startup(drv, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == 1);
	CHECK(g_send_conn == CONN_ID_B);
	CHECK(ds45_shim_data.stat_startups == 2);
	CHECK(g_os_calls == 0);

	/* and the one in slot 0 - where kshim_set_connection puts a lone pad */
	send_reset(2);
	drv_make(drv, 0x11);
	CHECK(shim_startup(drv, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == 1);
	CHECK(g_send_conn == CONN_ID_C);
	CHECK(ds45_shim_data.stat_startups == 3);
	CHECK(g_os_calls == 0);                        /* slot 0 is a DualSense too */
	return 0;
}

/* Every slot of the table is found by its handle and sent to its own id - the
 * first (where kshim_set_connection puts a lone pad) and the last included. */
static int test_glue_startup_every_slot_reaches_its_own_id(void)
{
	uint8_t drv[OBJ_LEN];
	int s;

	glue_reset();
	for (s = 0; s < DS45_SHIM_CONNS; s++)
		slot_set(s, (uint8_t)(0x11 + s), CONN_ID_A + (uint64_t)s);

	for (s = 0; s < DS45_SHIM_CONNS; s++) {
		send_reset(2);
		drv_make(drv, (uint8_t)(0x11 + s));

		CHECK(shim_startup(drv, 1) == SHIM_F1_OK);
		CHECK(g_send_calls == 1);
		CHECK(g_send_conn == CONN_ID_A + (uint64_t)s);
		CHECK(g_os_calls == 0);
	}
	CHECK(ds45_shim_data.stat_startups == DS45_SHIM_CONNS);
	return 0;
}

/* The cap through the real hook. The driver may call the start-up again and
 * again for one connection; the GET goes out SHIM_START_TRIES_MAX times and then
 * the hook answers OK without sending - it must not fall through to the
 * original (which would DS3-enable a DualSense) nor return an error (which could
 * make the driver drop a pad that did switch). The budget is per slot, slot 0
 * included, and re-arming a slot - zeroing its count, as kshim_set_connection
 * does before it publishes a connection - lets it send again. */
static int test_glue_startup_resend_is_capped_per_connection(void)
{
	uint8_t drv0[OBJ_LEN], drv3[OBJ_LEN];
	int i;

	glue_reset();
	slot_set(0, 0x11, CONN_ID_C);
	slot_set(3, 0x2A, CONN_ID_A);
	drv_make(drv0, 0x11);
	drv_make(drv3, 0x2A);

	for (i = 0; i < SHIM_START_TRIES_MAX + 10; i++)
		CHECK(shim_startup(drv0, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX);
	CHECK(g_send_conn == CONN_ID_C);
	CHECK(g_os_calls == 0);                              /* never the original */
	CHECK(ds45_shim_data.start_tries[0] == SHIM_START_TRIES_MAX);
	CHECK(ds45_shim_data.stat_startups == SHIM_START_TRIES_MAX);

	/* another connection has its own budget */
	CHECK(shim_startup(drv3, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == SHIM_START_TRIES_MAX + 1);
	CHECK(g_send_conn == CONN_ID_A);
	CHECK(ds45_shim_data.start_tries[3] == 1);

	/* re-armed, slot 0 sends again - and is capped again */
	ds45_shim_data.start_tries[0] = 0;
	for (i = 0; i < SHIM_START_TRIES_MAX + 10; i++)
		CHECK(shim_startup(drv0, 1) == SHIM_F1_OK);
	CHECK(g_send_calls == 2 * SHIM_START_TRIES_MAX + 1);
	CHECK(g_send_conn == CONN_ID_C);
	CHECK(g_os_calls == 0);
	return 0;
}

/* A connection the plugin did not record (a DualShock 3, a Move navigation
 * controller, anything not a DualSense) takes exactly the original path: the
 * original is called with the very same arguments and its result returned, no
 * send is made and the data block is not touched. */
static int test_glue_startup_non_dualsense_tail_calls_original(void)
{
	uint8_t drv[OBJ_LEN];
	ds45_shim_data_t before;
	int rc;

	glue_reset();
	slot_set(2, 0x2A, CONN_ID_A);
	drv_make(drv, 0x77);                           /* not in any slot */
	memcpy(&before, &ds45_shim_data, sizeof(before));

	rc = shim_startup(drv, 5);

	CHECK(rc == ORIG_STARTUP_RC);
	CHECK(g_os_calls == 1);
	CHECK(g_os_drv == (void *)drv);
	CHECK(g_os_arg1 == 5);
	CHECK(g_send_calls == 0);
	CHECK(memcmp(&before, &ds45_shim_data, sizeof(before)) == 0);
	return 0;
}

/* d->enabled == 0 makes the start-up behave exactly like the original, even for
 * a connection the plugin recorded as a DualSense. */
static int test_glue_startup_disabled_tail_calls_original(void)
{
	uint8_t drv[OBJ_LEN];

	glue_reset();
	slot_set(0, 0x2A, CONN_ID_A);
	ds45_shim_data.enabled = 0;
	drv_make(drv, 0x2A);

	CHECK(shim_startup(drv, 1) == ORIG_STARTUP_RC);
	CHECK(g_os_calls == 1);
	CHECK(g_send_calls == 0);
	CHECK(ds45_shim_data.stat_startups == 0);
	return 0;
}

/* The original returns early for arg1 == 0 (F1); the shim leaves that to it. */
static int test_glue_startup_arg1_zero_tail_calls_original(void)
{
	uint8_t drv[OBJ_LEN];

	glue_reset();
	slot_set(0, 0x2A, CONN_ID_A);
	drv_make(drv, 0x2A);

	CHECK(shim_startup(drv, 0) == ORIG_STARTUP_RC);
	CHECK(g_os_calls == 1);
	CHECK(g_os_arg1 == 0);
	CHECK(g_send_calls == 0);
	return 0;
}

/* A slot whose state is cleared (the connection went away) is not a DualSense,
 * whatever stale handle or id it still holds - a DS3 reusing the handle must
 * take the original path. And a NULL driver object never crashes. */
static int test_glue_startup_cleared_slot_and_null_drv(void)
{
	uint8_t drv[OBJ_LEN];

	glue_reset();
	slot_set(1, 0x2A, CONN_ID_A);
	ds45_shim_data.conn_state[1] = 0;
	drv_make(drv, 0x2A);

	CHECK(shim_startup(drv, 1) == ORIG_STARTUP_RC);
	CHECK(g_os_calls == 1);
	CHECK(g_send_calls == 0);

	slot_set(1, 0x2A, CONN_ID_A);
	CHECK(shim_startup(0, 1) == ORIG_STARTUP_RC);
	CHECK(g_os_calls == 2);
	CHECK(g_os_drv == 0);
	CHECK(g_send_calls == 0);
	return 0;
}

/* A DualSense slot with no connection id captured yet fails the start-up safely
 * (the pure function's guard): nothing is sent, the original is not run on the
 * DualSense, and the driver sees F1's error. */
static int test_glue_startup_slot_without_conn_id_fails_safe(void)
{
	uint8_t drv[OBJ_LEN];

	glue_reset();
	slot_set(4, 0x2A, 0);
	drv_make(drv, 0x2A);

	CHECK(shim_startup(drv, 1) == SHIM_F1_ERR);
	CHECK(g_send_calls == 0);
	CHECK(g_os_calls == 0);
	CHECK(ds45_shim_data.stat_startups == 0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* F2 glue (entry.c): it shares the slot lookup with F1, so guard it too       */
/* ------------------------------------------------------------------------ */

/* A DualSense recorded in any slot - the first (where kshim_set_connection puts a
 * lone pad) and the last included - has its 0x31 report translated; only that
 * slot is recorded, so it is the slot's own entry that is found. */
static int test_glue_input_translates_every_slot(void)
{
	uint8_t xport[16], dst[OBJ_LEN], want[DS3_REPORT_LEN];
	int s;

	for (s = 0; s < DS45_SHIM_CONNS; s++) {
		glue_reset();
		slot_set(s, 0x2A, CONN_ID_A);
		memset(xport, 0, sizeof(xport));
		xport[F2_HANDLE_OFF] = 0x2A;
		report_neutral(g_oi_report);
		ds3_neutral(want);
		memset(dst, 0, sizeof(dst));

		CHECK(shim_input(xport, dst, 64, 0) == DS3_REPORT_LEN);
		CHECK(g_oi_calls == 1);
		CHECK(memcmp(dst, want, DS3_REPORT_LEN) == 0);
		CHECK(ds45_shim_data.stat_translated == 1);
	}
	return 0;
}

static int test_glue_input_unknown_handle_untouched(void)
{
	uint8_t xport[16], dst[OBJ_LEN];
	int rc;

	glue_reset();
	slot_set(6, 0x2A, CONN_ID_A);
	memset(xport, 0, sizeof(xport));
	xport[F2_HANDLE_OFF] = 0x77;                   /* a DS3's connection */
	report_neutral(g_oi_report);
	memset(dst, 0, sizeof(dst));

	rc = shim_input(xport, dst, 64, 0);

	CHECK(g_oi_calls == 1);
	CHECK(rc == ORIG_INPUT_LEN);                   /* the original's result */
	CHECK(memcmp(dst, g_oi_report, ORIG_INPUT_LEN) == 0);
	CHECK(ds45_shim_data.stat_translated == 0);
	CHECK(ds45_shim_data.stat_passed == 0);        /* never even entered the filter */
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
		test_startup_sends_feature_05_get,
		test_startup_uses_the_slots_own_conn_id,
		test_startup_send_success_codes,
		test_startup_send_fails,
		test_startup_without_send_routine,
		test_startup_without_conn_id,
		test_startup_bad_slot_fails_safe,
		test_startup_send_is_capped_per_slot,
		test_startup_failed_sends_spend_the_budget,
		test_glue_startup_sends_to_the_slots_conn_id,
		test_glue_startup_every_slot_reaches_its_own_id,
		test_glue_startup_resend_is_capped_per_connection,
		test_glue_startup_non_dualsense_tail_calls_original,
		test_glue_startup_disabled_tail_calls_original,
		test_glue_startup_arg1_zero_tail_calls_original,
		test_glue_startup_cleared_slot_and_null_drv,
		test_glue_startup_slot_without_conn_id_fails_safe,
		test_glue_input_translates_every_slot,
		test_glue_input_unknown_handle_untouched,
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
