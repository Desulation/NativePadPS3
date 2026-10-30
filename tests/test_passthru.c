/* SOURCES: lv2.c kernel.c btcheck.c passthru.c */
#include <stdint.h>
#include <string.h>
#include <cell/pad.h>
#include "fakes.h"
#include "ds45pad.h"
#include "lv2.h"
#include "kernel.h"
#include "btcheck.h"

/* Bluetooth passthrough gated on the kernel record and the cross-check.
 * The built-in table resolves to its legacy entry (no cache, empty scan
 * pattern), whose slot is LEG_SLOT and report offset 0xE8. */

ds45_config_t g_cfg;
ds45_pad_t    g_pads[DS45_MAX_PADS];

#define B         DS45_LV2_BASE
#define LEG_SLOT  0x800000000034C530ULL
#define OFF       0xE8
#define BT_WORK   (B + 0x500000)
#define W0_ADDR   (BT_WORK + OFF)
#define W1_ADDR   (BT_WORK + OFF + 8)
#define SRC_PORT  6

#define EXPECT(e) do { int l_ = (e); if (l_) return l_; } while (0)

static void put64(uint64_t a, uint64_t v)
{
	uint8_t b[8];
	int i;
	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	fake_lv2_set(a, b, 8);
}

/* A1 01 lx ly rx ry b0 b1; hat neutral (8), no buttons */
static uint64_t mk_w0(uint8_t lx)
{
	return 0xA101000000000800ULL | ((uint64_t)lx << 40) |
	       (0x80ULL << 32) | (0x80ULL << 24) | (0x80ULL << 16);
}

static void put_report(uint8_t lx)
{
	put64(W0_ADDR, mk_w0(lx));
	put64(W1_ADDR, 0);
}

static CellPadData mk_vsh(uint8_t lx)
{
	CellPadData d;
	memset(&d, 0, sizeof(d));
	d.len = 24;
	d.button[DS45_BTN_ANALOG_LEFT_X]  = lx;
	d.button[DS45_BTN_ANALOG_LEFT_Y]  = 0x80;
	d.button[DS45_BTN_ANALOG_RIGHT_X] = 0x80;
	d.button[DS45_BTN_ANALOG_RIGHT_Y] = 0x80;
	return d;
}

static void set_connected(int on)
{
	fake_pad_info.port_status[SRC_PORT] = on ? DS45_STATUS_CONNECTED : 0;
}

/* Fresh fakes, fresh plugin state, a generic pad on SRC_PORT. */
static void setup(void)
{
	int i;

	fake_reset_all();
	memset(g_pads, 0, sizeof(g_pads));
	for (i = 0; i < DS45_MAX_PADS; i++) {
		g_pads[i].handle = g_pads[i].port = g_pads[i].src_port = -1;
		g_pads[i].dev_id = -1;
		g_pads[i].c_pipe = g_pads[i].i_pipe = g_pads[i].o_pipe = -1;
	}
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.enable = 1;
	g_cfg.mode = DS45_MODE_BT;
	g_cfg.bt_source_port = -1;
	g_cfg.max_pads = 1;
	g_cfg.poll_ms = 10;
	g_cfg.bt_ps_code = -1;
	set_connected(1);
	fake_now_us = 1000000;
	passthru_init();
}

/* setup + legacy resolution + a valid slot and report, pad bound */
static int setup_bound(void)
{
	setup();
	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	put64(LEG_SLOT, BT_WORK);
	put_report(0x80);
	CHECK(passthru_scan() == 0);                /* binds, does not register */
	CHECK(g_pads[0].in_use && g_pads[0].link == DS45_LINK_BT);
	CHECK(g_pads[0].src_port == SRC_PORT && g_pads[0].handle < 0);
	CHECK(fake_ldd_registers == 0);
	return 0;
}

/* One poll of a raw report with stick x, then VSH reading the same state. */
static void round_match(int i)
{
	CellPadData d;
	uint8_t x = (uint8_t)(0x40 + (i & 0x3F));

	put_report(x);
	fake_now_us += 10000;
	passthru_poll();
	d = mk_vsh(x);
	passthru_observe_vsh(SRC_PORT, &d);
}

/* VSH reads something the raw report never showed. */
static void round_mismatch(int i)
{
	CellPadData d;

	put_report((uint8_t)(0x40 + (i & 0x3F)));
	fake_now_us += 10000;
	passthru_poll();
	d = mk_vsh((uint8_t)(0x10 + (i & 1)));
	passthru_observe_vsh(SRC_PORT, &d);
}

static int any_bt_pad(void)
{
	int i;
	for (i = 0; i < DS45_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == DS45_LINK_BT)
			return 1;
	return 0;
}

static int locks_clean(void)
{
	CHECK(fake_mutex_depth == 0);
	CHECK(fake_mutex_errors == 0);
	return 0;
}

static int test_no_virtual_pad_before_pass(void)
{
	int i;

	EXPECT(setup_bound());
	for (i = 0; i < BTCHECK_MATCHES_REQUIRED - 1; i++) {
		round_match(i);
		CHECK(passthru_scan() == 0);
		CHECK(fake_ldd_registers == 0);
	}
	CHECK(passthru_btcheck_state() == BTCHECK_PENDING);
	CHECK(fake_ldd_inserts == 0);               /* nothing injected before a pass */

	round_match(BTCHECK_MATCHES_REQUIRED - 1);  /* the 50th */
	CHECK(passthru_btcheck_state() == BTCHECK_PASSED);
	CHECK(fake_ldd_registers == 0);             /* registration is the scan's job */
	CHECK(passthru_scan() == 1);
	CHECK(fake_ldd_registers == 1);
	CHECK(g_pads[0].handle >= 0);

	/* once registered, the poll injects */
	round_match(BTCHECK_MATCHES_REQUIRED);
	CHECK(fake_ldd_inserts > 0);
	/* and a later scan neither registers again nor binds a second pad */
	CHECK(passthru_scan() == 0);
	CHECK(fake_ldd_registers == 1);
	CHECK(!g_pads[1].in_use);
	EXPECT(locks_clean());
	CHECK(fake_lv2_out_of_bounds == 0);
	return 0;
}

static int test_none_registers_nothing(void)
{
	static const ds45_kernel_t only_fp[1] = {
		{ "fp", B + 0x123400, 0x1234, B + 0x345670, 0xE8 }
	};

	setup();
	CHECK(kernel_resolve_from(only_fp, 1, NULL, NULL) == DS45_KMATCH_NONE);
	CHECK(kernel_active() == NULL);
	fake_lv2_reset();                           /* count from here */

	CHECK(passthru_scan() == 0);
	CHECK(!any_bt_pad());
	passthru_poll();
	CHECK(passthru_scan() == 0);
	CHECK(!any_bt_pad());
	CHECK(fake_ldd_registers == 0);
	CHECK(fake_lv2_peek_count == 0);            /* no LV2 read without a record */
	CHECK(passthru_raw_samples() == 0);
	EXPECT(locks_clean());
	return 0;
}

static int test_fail_steps_aside(void)
{
	int i;

	/* a cache file is present (unparseable, so legacy resolves) */
	setup();
	fake_file_set(DS45_KERNEL_CACHE_FILE, "junk\n");
	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	put64(LEG_SLOT, BT_WORK);
	put_report(0x80);
	CHECK(passthru_scan() == 0);
	CHECK(any_bt_pad());

	for (i = 0; i <= BTCHECK_MISMATCH_LIMIT; i++)
		round_mismatch(i);
	CHECK(passthru_btcheck_state() == BTCHECK_FAILED);

	CHECK(passthru_scan() == 0);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	CHECK(kernel_active() == NULL);
	CHECK(!any_bt_pad());                       /* released */

	/* steps aside for good: the pad is still there, nothing is adopted */
	fake_lv2_reset();
	for (i = 0; i < 3; i++) {
		passthru_poll();
		CHECK(passthru_scan() == 0);
	}
	CHECK(!any_bt_pad());
	CHECK(fake_ldd_registers == 0);
	CHECK(fake_lv2_peek_count == 0);
	EXPECT(locks_clean());
	return 0;
}

/* After a pass: the pad goes away but the raw report keeps changing. */
static int test_fail_after_pass_unregisters(void)
{
	int i;

	EXPECT(setup_bound());
	for (i = 0; i < BTCHECK_MATCHES_REQUIRED; i++)
		round_match(i);
	CHECK(passthru_scan() == 1);
	CHECK(fake_ldd_registers == 1);

	set_connected(0);
	fake_now_us += 10000;
	passthru_poll();                            /* pad torn down, freeze watch on */
	CHECK(!any_bt_pad());
	CHECK(fake_ldd_unregisters == 1);
	for (i = 0; i < 30; i++) {                  /* 300 ms of a changing buffer */
		put_report((uint8_t)(0x10 + i));
		fake_now_us += 10000;
		passthru_poll();
	}
	CHECK(passthru_btcheck_state() == BTCHECK_FAILED);
	set_connected(1);
	CHECK(passthru_scan() == 0);
	CHECK(kernel_active() == NULL);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 1);
	CHECK(!any_bt_pad());
	CHECK(passthru_scan() == 0);
	CHECK(fake_ldd_registers == 1);
	EXPECT(locks_clean());
	return 0;
}

static int g_tear;

/* Every read of w1 is followed by a kernel write to w0. */
static void tear_w0(uint64_t addr)
{
	if (addr == W1_ADDR) {
		g_tear++;
		put64(W0_ADDR, mk_w0((uint8_t)(0x20 + g_tear)));
	}
}

static int test_torn_read_dropped(void)
{
	EXPECT(setup_bound());
	CHECK(passthru_raw_samples() == 0);

	fake_lv2_on_peek = tear_w0;
	passthru_poll();
	passthru_poll();
	CHECK(g_tear >= 2);                         /* the tear really happened */
	CHECK(passthru_raw_samples() == 0);         /* both samples dropped */

	fake_lv2_on_peek = NULL;
	passthru_poll();
	CHECK(passthru_raw_samples() == 1);
	CHECK(fake_lv2_out_of_bounds == 0);
	EXPECT(locks_clean());
	return 0;
}

static int test_null_slot_retried(void)
{
	int i;

	setup();
	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	put_report(0x80);                           /* slot itself still 0 */
	CHECK(passthru_scan() == 0);
	CHECK(any_bt_pad());
	for (i = 0; i < 3; i++) {
		passthru_poll();
		CHECK(passthru_scan() == 0);            /* each tick retries */
	}
	CHECK(passthru_raw_samples() == 0);

	/* out-of-window and unaligned pointers are not used either */
	put64(LEG_SLOT, DS45_LV2_BOUND);
	CHECK(passthru_scan() == 0);
	passthru_poll();
	put64(LEG_SLOT, BT_WORK + 4);
	CHECK(passthru_scan() == 0);
	passthru_poll();
	put64(LEG_SLOT, 0x0000000000500000ULL);
	CHECK(passthru_scan() == 0);
	passthru_poll();
	CHECK(passthru_raw_samples() == 0);
	CHECK(fake_lv2_out_of_bounds == 0);

	/* the Bluetooth stack comes up */
	put64(LEG_SLOT, BT_WORK);
	CHECK(passthru_scan() == 0);
	passthru_poll();
	CHECK(passthru_raw_samples() == 1);

	/* nothing about the failure was cached */
	CHECK(fake_file_size(DS45_KERNEL_CACHE_FILE) == -1);
	CHECK(fake_file_deleted(DS45_KERNEL_CACHE_FILE) == 0);
	CHECK(kernel_active() != NULL);
	CHECK(fake_lv2_out_of_bounds == 0);
	EXPECT(locks_clean());
	return 0;
}

static int test_passthru_scan_waits_for_resolution(void)
{
	CellPadData d = mk_vsh(0x42);

	setup();
	/* no record yet (the load-time scan is still running) */
	CHECK(kernel_resolve_from(NULL, 0, NULL, NULL) == DS45_KMATCH_NONE);
	put64(LEG_SLOT, BT_WORK);
	put_report(0x80);
	CHECK(passthru_scan() == 0);
	CHECK(!any_bt_pad());

	CHECK(kernel_resolve(NULL) == DS45_KMATCH_LEGACY);
	CHECK(passthru_scan() == 0);                /* first scan after it */
	CHECK(any_bt_pad());
	CHECK(g_pads[0].src_port == SRC_PORT);
	/* the cross-check now watches that port */
	passthru_observe_vsh(SRC_PORT, &d);
	CHECK(passthru_btcheck_vsh_samples() == 1);
	EXPECT(locks_clean());
	return 0;
}

static int test_vsh_sample_dropped_on_contention(void)
{
	CellPadData d1 = mk_vsh(0x42), d2 = mk_vsh(0x43);

	EXPECT(setup_bound());
	/* before any bind nothing is watched: port 0 reads never count */
	setup();
	passthru_observe_vsh(0, &d1);
	CHECK(passthru_btcheck_vsh_samples() == 0);

	EXPECT(setup_bound());
	fake_mutex_busy = 1;                        /* the poll thread holds it */
	passthru_observe_vsh(SRC_PORT, &d1);
	fake_mutex_busy = 0;
	CHECK(passthru_btcheck_vsh_samples() == 0); /* dropped, not blocked */
	passthru_observe_vsh(SRC_PORT, &d2);
	CHECK(passthru_btcheck_vsh_samples() == 1);
	passthru_observe_vsh(SRC_PORT, NULL);
	CHECK(passthru_btcheck_vsh_samples() == 1);
	EXPECT(locks_clean());

	passthru_shutdown();
	CHECK(fake_mutex_live == 0);
	passthru_observe_vsh(SRC_PORT, &d1);        /* after shutdown: ignored */
	CHECK(fake_mutex_errors == 0);
	return 0;
}

int run_tests(void)
{
	EXPECT(test_no_virtual_pad_before_pass());
	EXPECT(test_none_registers_nothing());
	EXPECT(test_fail_steps_aside());
	EXPECT(test_fail_after_pass_unregisters());
	EXPECT(test_torn_read_dropped());
	EXPECT(test_null_slot_retried());
	EXPECT(test_passthru_scan_waits_for_resolution());
	EXPECT(test_vsh_sample_dropped_on_contention());
	return 0;
}
