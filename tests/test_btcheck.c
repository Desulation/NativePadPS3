/* SOURCES: btcheck.c */
#include <stdint.h>
#include <string.h>
#include "fakes.h"
#include "btcheck.h"

/* Pure logic: no LV2 reads, no clock of its own (time is passed in). */

#define PORT 0

static ds45_btraw_t mk_raw(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry,
			   uint16_t d1, uint16_t d2)
{
	ds45_btraw_t r;
	memset(&r, 0, sizeof(r));
	r.lx = lx; r.ly = ly; r.rx = rx; r.ry = ry;
	r.hat = 8;
	r.d1 = d1; r.d2 = d2;
	return r;
}

static CellPadData mk_vsh(uint16_t lx, uint16_t ly, uint16_t rx, uint16_t ry,
			  uint16_t d1, uint16_t d2)
{
	CellPadData d;
	memset(&d, 0, sizeof(d));
	d.len = 24;
	d.button[DS45_BTN_ANALOG_LEFT_X]  = lx;
	d.button[DS45_BTN_ANALOG_LEFT_Y]  = ly;
	d.button[DS45_BTN_ANALOG_RIGHT_X] = rx;
	d.button[DS45_BTN_ANALOG_RIGHT_Y] = ry;
	d.button[DS45_BTN_DIGITAL1] = d1;
	d.button[DS45_BTN_DIGITAL2] = d2;
	return d;
}

/* One matching round: raw (connected) then the same state seen by VSH. The
 * stick x value is varied so every VSH observation is a counted change. */
static btcheck_state_t round_match(int i, uint64_t t)
{
	uint8_t x = (uint8_t)(0x40 + (i & 0x3F));
	ds45_btraw_t r = mk_raw(x, 0x80, 0x80, 0x80, 0, 0);
	CellPadData  d = mk_vsh(x, 0x80, 0x80, 0x80, 0, 0);
	btcheck_state_t s = btcheck_feed_raw(&r, 1, t);
	btcheck_observe_vsh(PORT, &d);
	return s;
}

static int test_decode_rejects_report_ids(void)
{
	ds45_btraw_t o;
	CHECK(bt_report_decode(0xA131808080800800ULL, 0, &o) == -1);
	CHECK(bt_report_decode(0xA111808080800800ULL, 0, &o) == -1);
	CHECK(bt_report_decode(0xA001808080800800ULL, 0, &o) == -1);
	CHECK(bt_report_decode(0xA101808080800800ULL, 0, NULL) == -1);
	memset(&o, 0xEE, sizeof(o));
	CHECK(bt_report_decode(0xA101808080800800ULL, 0, &o) == 0);
	CHECK(o.lx == 0x80 && o.ly == 0x80 && o.rx == 0x80 && o.ry == 0x80);
	CHECK(o.hat == 8);
	CHECK(o.d1 == 0 && o.d2 == 0 && o.ps == 0);
	CHECK(o.l2 == 0 && o.r2 == 0);
	return 0;
}

static int test_decode_rejects_bad_hat(void)
{
	ds45_btraw_t o;
	uint64_t hat;
	CHECK(bt_report_decode(0xA101808080800900ULL, 0, &o) == -1);   /* hat 9 */
	CHECK(bt_report_decode(0xA101808080800F00ULL, 0, &o) == -1);   /* hat 15 */
	for (hat = 0; hat <= 8; hat++) {
		CHECK(bt_report_decode(0xA101808080800000ULL | (hat << 8), 0, &o) == 0);
		CHECK(o.hat == hat);
	}
	/* high nibble of b0 is face buttons, not part of the hat */
	CHECK(bt_report_decode(0xA10180808080F800ULL, 0, &o) == 0);
	CHECK(o.hat == 8);
	return 0;
}

static int test_decode_fields(void)
{
	ds45_btraw_t o;
	/* sticks 11 22 33 44; b0 = Triangle|Cross | hat 1 (Up+Right);
	 * b1 = R1|Options|R3; b2 = PS; l2=0x55 r2=0x66 */
	uint64_t w0 = 0xA101112233448101ULL & ~0xFFFFULL;
	uint64_t w1;
	w0 |= (uint64_t)(0xA0 | 0x01) << 8;   /* b0: Triangle(0x80)|Cross(0x20)|hat 1 */
	w0 |= 0x02 | 0x20 | 0x80;             /* b1: R1, Options, R3 */
	w1 = ((uint64_t)0x01 << 56) | ((uint64_t)0x55 << 48) | ((uint64_t)0x66 << 40);
	CHECK(bt_report_decode(w0, w1, &o) == 0);
	CHECK(o.lx == 0x11 && o.ly == 0x22 && o.rx == 0x33 && o.ry == 0x44);
	CHECK(o.hat == 1);
	CHECK(o.l2 == 0x55 && o.r2 == 0x66);
	CHECK(o.ps == 1);
	CHECK(o.d1 == (DS45_CTRL_PS | DS45_CTRL_UP | DS45_CTRL_RIGHT |
		       DS45_CTRL_START | DS45_CTRL_R3));
	CHECK(o.d2 == (DS45_CTRL_TRIANGLE | DS45_CTRL_CROSS | DS45_CTRL_R1));
	/* remaining face/shoulder bits, hat 6 (Left) */
	w0 = 0xA101808080800000ULL | ((uint64_t)(0x10 | 0x40 | 0x06) << 8) |
	     (0x01 | 0x04 | 0x08 | 0x10 | 0x40);
	CHECK(bt_report_decode(w0, 0, &o) == 0);
	CHECK(o.d1 == (DS45_CTRL_LEFT | DS45_CTRL_SELECT | DS45_CTRL_L3));
	CHECK(o.d2 == (DS45_CTRL_SQUARE | DS45_CTRL_CIRCLE | DS45_CTRL_L1 |
		       DS45_CTRL_L2 | DS45_CTRL_R2));
	CHECK(o.ps == 0);
	return 0;
}

static int test_pass_after_50_matches(void)
{
	int i;
	btcheck_reset(PORT);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	for (i = 0; i < 49; i++) {
		round_match(i, 1000 + (uint64_t)i * 10000);
		CHECK(btcheck_state() == BTCHECK_PENDING);
	}
	round_match(49, 1000 + 49 * 10000);
	CHECK(btcheck_state() == BTCHECK_PASSED);
	CHECK(btcheck_vsh_samples_seen() == 50);
	/* stays passed */
	round_match(50, 1000 + 50 * 10000);
	CHECK(btcheck_state() == BTCHECK_PASSED);
	return 0;
}

static int test_tolerance_2(void)
{
	ds45_btraw_t r = mk_raw(100, 100, 100, 100, 0, 0);
	CellPadData  d;
	int i, k;

	/* off by exactly 2 on every stick, either direction: all match */
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000);
	for (i = 0; i < 50; i++) {
		d = (i & 1) ? mk_vsh(98, 102, 98, 102, 0, 0)
			    : mk_vsh(102, 98, 102, 98, 0, 0);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PASSED);

	/* off by 3 on any single stick, either direction, is a mismatch. Two of
	 * them plus nine more mismatches reach 11 -> FAILED; had either of the
	 * first two matched, only nine would count and the state stays PENDING. */
	for (i = 0; i < 4; i++) {
		uint16_t v[4];
		btcheck_reset(PORT);
		btcheck_feed_raw(&r, 1, 1000);
		v[0] = v[1] = v[2] = v[3] = 100;
		v[i] = 103;
		d = mk_vsh(v[0], v[1], v[2], v[3], 0, 0);
		btcheck_observe_vsh(PORT, &d);
		v[i] = 97;
		d = mk_vsh(v[0], v[1], v[2], v[3], 0, 0);
		btcheck_observe_vsh(PORT, &d);
		CHECK(btcheck_state() == BTCHECK_PENDING);
		for (k = 0; k < 9; k++) {
			d = mk_vsh(v[0], v[1], v[2], v[3], 0, (uint16_t)(k + 1));
			btcheck_observe_vsh(PORT, &d);
		}
		CHECK(btcheck_state() == BTCHECK_FAILED);
	}
	return 0;
}

static int test_buttons_must_match(void)
{
	ds45_btraw_t r = mk_raw(128, 128, 128, 128, DS45_CTRL_UP, DS45_CTRL_CROSS);
	CellPadData  d;
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000);
	/* d2 differs: mismatch; matches would otherwise pass at 50 */
	d = mk_vsh(128, 128, 128, 128, DS45_CTRL_UP, DS45_CTRL_CIRCLE);
	btcheck_observe_vsh(PORT, &d);
	d = mk_vsh(128, 128, 128, 128, DS45_CTRL_DOWN, DS45_CTRL_CROSS);
	btcheck_observe_vsh(PORT, &d);
	d = mk_vsh(128, 128, 128, 128, DS45_CTRL_UP, DS45_CTRL_CROSS);
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	return 0;
}

static int test_ps_bit_ignored(void)
{
	int i;
	ds45_btraw_t r = mk_raw(0x80, 0x80, 0x80, 0x80, DS45_CTRL_PS | DS45_CTRL_UP, 0);
	r.ps = 1;

	/* raw has PS, VSH does not */
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000);
	for (i = 0; i < 50; i++) {
		/* wiggle within tolerance so each observation is a change */
		CellPadData d = mk_vsh((uint16_t)(0x80 + (i & 1)), 0x80, 0x80, 0x80,
				       DS45_CTRL_UP, 0);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PASSED);

	/* VSH has PS, raw does not */
	btcheck_reset(PORT);
	r = mk_raw(0x80, 0x80, 0x80, 0x80, DS45_CTRL_UP, 0);
	btcheck_feed_raw(&r, 1, 1000);
	for (i = 0; i < 50; i++) {
		CellPadData d = mk_vsh((uint16_t)(0x80 + (i & 1)), 0x80, 0x80, 0x80,
				       DS45_CTRL_UP | DS45_CTRL_PS, 0);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PASSED);
	return 0;
}

static int test_fail_after_11_mismatches(void)
{
	int i;
	ds45_btraw_t r = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000);
	for (i = 0; i < 10; i++) {
		CellPadData d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
		CHECK(btcheck_state() == BTCHECK_PENDING);
	}
	{
		CellPadData d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, 100);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_FAILED);
	/* feed_raw reports the failure too, and it sticks */
	CHECK(btcheck_feed_raw(&r, 1, 2000) == BTCHECK_FAILED);
	CHECK(btcheck_state() == BTCHECK_FAILED);
	return 0;
}

static int test_mismatch_after_pass_ignored(void)
{
	int i;
	ds45_btraw_t r = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	btcheck_reset(PORT);
	for (i = 0; i < 50; i++) round_match(i, 1000 + (uint64_t)i);
	CHECK(btcheck_state() == BTCHECK_PASSED);
	btcheck_feed_raw(&r, 1, 5000);
	for (i = 0; i < 30; i++) {
		CellPadData d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PASSED);
	return 0;
}

static ds45_btraw_t distinct_raw(int k)    /* never within tolerance of (10,10,10,10) */
{
	return mk_raw((uint8_t)(200 + k), 200, 200, 200, 0, (uint16_t)k);
}

static int test_history_window(void)
{
	int i;
	CellPadData d;
	ds45_btraw_t a = mk_raw(10, 10, 10, 10, 0, 0);
	ds45_btraw_t b;

	/* `a` plus 7 later distinct states: still one of the last 8, so it
	 * matches. (One match, then 10 mismatches: not past the limit. Had the
	 * first been a mismatch, 11 would FAIL.) */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	for (i = 0; i < BTCHECK_RAW_HISTORY - 1; i++) {
		b = distinct_raw(i);
		btcheck_feed_raw(&b, 1, 2000 + (uint64_t)i);
	}
	d = mk_vsh(10, 10, 10, 10, 0, 0);
	btcheck_observe_vsh(PORT, &d);
	for (i = 0; i < 10; i++) {
		d = mk_vsh(50, 50, 50, 50, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);

	/* one more distinct state pushes `a` out: it now mismatches */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	for (i = 0; i < BTCHECK_RAW_HISTORY; i++) {
		b = distinct_raw(i);
		btcheck_feed_raw(&b, 1, 2000 + (uint64_t)i);
	}
	for (i = 0; i < 11; i++) {
		d = mk_vsh(10, 10, 10, 10, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_FAILED);
	return 0;
}

static int test_repeated_raw_does_not_evict_history(void)
{
	int i;
	CellPadData d;
	ds45_btraw_t a = mk_raw(10, 10, 10, 10, 0, 0);
	ds45_btraw_t b = distinct_raw(0);

	/* 20 identical raw samples after an older distinct one: the older
	 * state is still in the history, so a lagging VSH read of it matches */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	for (i = 0; i < 20; i++)
		btcheck_feed_raw(&b, 1, 2000 + (uint64_t)i * 10000);
	d = mk_vsh(10, 10, 10, 10, 0, 0);
	btcheck_observe_vsh(PORT, &d);
	/* it was a match (not a mismatch): 10 further mismatches stay PENDING */
	for (i = 0; i < 10; i++) {
		d = mk_vsh(50, 50, 50, 50, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);

	/* the same through to PASSED: 50 lagging reads of `a` match */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	for (i = 0; i < 20; i++)
		btcheck_feed_raw(&b, 1, 2000 + (uint64_t)i * 10000);
	for (i = 0; i < 50; i++) {
		d = mk_vsh((uint16_t)(10 + (i & 1)), 10, 10, 10, 0, 0);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PASSED);
	return 0;
}

static int test_unchanged_vsh_samples_not_counted(void)
{
	int i;
	CellPadData d;
	ds45_btraw_t r = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000);
	d = mk_vsh(0x80, 0x80, 0x80, 0x80, 0, 0);
	for (i = 0; i < 200; i++)
		btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 1);   /* first one counts */
	CHECK(btcheck_state() == BTCHECK_PENDING);
	/* only pad data that feeds the comparison matters for "changed" */
	d.button[DS45_BTN_SENSOR_X] = 0x1234;
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 1);
	d.button[DS45_BTN_DIGITAL2] = DS45_CTRL_CROSS;
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 2);
	/* back to the earlier value is a change again */
	d.button[DS45_BTN_DIGITAL2] = 0;
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 3);
	/* ... and PS flipping alone is a change too (it is part of the sample) */
	d.button[DS45_BTN_DIGITAL1] = DS45_CTRL_PS;
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 4);
	return 0;
}

static int test_other_ports_and_empty_ignored(void)
{
	CellPadData d = mk_vsh(1, 2, 3, 4, 0, 0);
	btcheck_reset(2);
	btcheck_observe_vsh(0, &d);
	btcheck_observe_vsh(6, &d);
	btcheck_observe_vsh(99, &d);
	btcheck_observe_vsh(2, NULL);
	CHECK(btcheck_vsh_samples_seen() == 0);
	d.len = 0;                       /* no data this poll */
	btcheck_observe_vsh(2, &d);
	CHECK(btcheck_vsh_samples_seen() == 0);
	d.len = 24;
	btcheck_observe_vsh(2, &d);
	CHECK(btcheck_vsh_samples_seen() == 1);
	return 0;
}

static int test_no_raw_yet_is_not_a_mismatch(void)
{
	int i;
	btcheck_reset(PORT);
	for (i = 0; i < 40; i++) {
		CellPadData d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);
	CHECK(btcheck_vsh_samples_seen() == 40);
	return 0;
}

static int test_freeze_after_disconnect(void)
{
	ds45_btraw_t a = mk_raw(1, 2, 3, 4, 0, 0);
	ds45_btraw_t b = mk_raw(9, 2, 3, 4, 0, 0);

	/* change 250 ms after the disconnect: FAILED */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	CHECK(btcheck_feed_raw(&a, 0, 2000000) == BTCHECK_PENDING);   /* t0 */
	CHECK(btcheck_feed_raw(&a, 0, 2100000) == BTCHECK_PENDING);   /* unchanged */
	CHECK(btcheck_feed_raw(&b, 0, 2250000) == BTCHECK_FAILED);
	CHECK(btcheck_state() == BTCHECK_FAILED);

	/* change 150 ms after: still pending */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	CHECK(btcheck_feed_raw(&b, 0, 2150000) == BTCHECK_PENDING);
	CHECK(btcheck_state() == BTCHECK_PENDING);

	/* exactly 200 ms is within the allowance; just past it is not */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	CHECK(btcheck_feed_raw(&b, 0, 2000000 + BTCHECK_FREEZE_US) == BTCHECK_PENDING);
	CHECK(btcheck_feed_raw(&a, 0, 2000000 + BTCHECK_FREEZE_US + 1) == BTCHECK_FAILED);

	/* the watch is anchored at the FIRST disconnected call, not the latest */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	btcheck_feed_raw(&a, 0, 2190000);
	CHECK(btcheck_feed_raw(&b, 0, 2250000) == BTCHECK_FAILED);

	/* a still-frozen raw never fails, however long the pad stays away */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	CHECK(btcheck_feed_raw(&a, 0, 9000000) == BTCHECK_PENDING);

	/* changes while connected are normal, and reconnecting cancels the watch */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	btcheck_feed_raw(&a, 1, 2100000);
	CHECK(btcheck_feed_raw(&b, 1, 3000000) == BTCHECK_PENDING);
	CHECK(btcheck_feed_raw(&a, 1, 3100000) == BTCHECK_PENDING);

	/* never seen connected: nothing to watch */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 0, 1000000);
	CHECK(btcheck_feed_raw(&b, 0, 5000000) == BTCHECK_PENDING);

	/* a passed result is revoked by a raw that keeps changing while away */
	{
		int i;
		btcheck_reset(PORT);
		for (i = 0; i < 50; i++) round_match(i, 1000 + (uint64_t)i);
		CHECK(btcheck_state() == BTCHECK_PASSED);
		btcheck_feed_raw(&a, 0, 2000000);
		CHECK(btcheck_feed_raw(&b, 0, 2300000) == BTCHECK_FAILED);
	}
	return 0;
}

static int test_disconnect_while_pending_resets(void)
{
	int i;
	CellPadData d;
	ds45_btraw_t a = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	ds45_btraw_t b = mk_raw(0x90, 0x80, 0x80, 0x80, 0, 0);

	/* 49 matches, disconnect, 49 more: not 50 (counters were reset) */
	btcheck_reset(PORT);
	for (i = 0; i < 49; i++) round_match(i, 1000 + (uint64_t)i);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	CHECK(btcheck_feed_raw(&a, 0, 2000000) == BTCHECK_PENDING);
	for (i = 0; i < 49; i++) round_match(i + 100, 3000000 + (uint64_t)i);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	round_match(200, 3100000);
	CHECK(btcheck_state() == BTCHECK_PASSED);    /* 50 since reconnect */

	/* 10 mismatches, disconnect, 10 more: not FAILED */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	for (i = 0; i < 10; i++) {
		d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);
	btcheck_feed_raw(&a, 0, 2000);
	btcheck_feed_raw(&a, 1, 3000);
	for (i = 0; i < 10; i++) {
		d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)(100 + i));
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);

	/* the freeze watch survives the reset */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000000);
	btcheck_feed_raw(&a, 0, 2000000);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	CHECK(btcheck_feed_raw(&b, 0, 2300000) == BTCHECK_FAILED);

	/* stale raw history is dropped too: pre-disconnect raw can't be matched
	 * after reconnect */
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	btcheck_feed_raw(&a, 0, 2000);
	btcheck_feed_raw(&b, 1, 3000);
	d = mk_vsh(0x80, 0x80, 0x80, 0x80, 0, 0);       /* only `a` would match */
	for (i = 0; i < 11; i++) {
		d.button[DS45_BTN_DIGITAL2] = (uint16_t)i;
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_FAILED);
	return 0;
}

static int test_vsh_comparison_gated_while_disconnected(void)
{
	int i;
	ds45_btraw_t a = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	btcheck_reset(PORT);
	btcheck_feed_raw(&a, 1, 1000);
	btcheck_feed_raw(&a, 0, 2000);
	for (i = 0; i < 30; i++) {
		CellPadData d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(PORT, &d);
	}
	CHECK(btcheck_state() == BTCHECK_PENDING);   /* neither failed ... */
	CHECK(btcheck_vsh_samples_seen() == 30);     /* ... nor uncounted */
	return 0;
}

static int test_vsh_counter_survives_failure(void)
{
	int i;
	CellPadData d;
	ds45_btraw_t r = mk_raw(0x80, 0x80, 0x80, 0x80, 0, 0);
	btcheck_reset(3);
	btcheck_feed_raw(&r, 1, 1000);
	for (i = 0; i < 11; i++) {
		d = mk_vsh(0x10, 0x80, 0x80, 0x80, 0, (uint16_t)i);
		btcheck_observe_vsh(3, &d);
	}
	CHECK(btcheck_state() == BTCHECK_FAILED);
	CHECK(btcheck_vsh_samples_seen() == 11);
	d = mk_vsh(0x11, 0x80, 0x80, 0x80, 0, 0);
	btcheck_observe_vsh(3, &d);
	CHECK(btcheck_vsh_samples_seen() == 12);
	btcheck_observe_vsh(3, &d);                  /* unchanged: not counted */
	CHECK(btcheck_vsh_samples_seen() == 12);
	d = mk_vsh(0x12, 0x80, 0x80, 0x80, 0, 0);
	btcheck_observe_vsh(0, &d);                  /* source port survived */
	CHECK(btcheck_vsh_samples_seen() == 12);
	d = mk_vsh(0x12, 0x80, 0x80, 0x80, 0, 0);
	btcheck_observe_vsh(3, &d);
	CHECK(btcheck_vsh_samples_seen() == 13);
	/* a freeze failure keeps them as well */
	btcheck_reset(PORT);
	btcheck_feed_raw(&r, 1, 1000000);
	btcheck_feed_raw(&r, 0, 2000000);
	d = mk_vsh(1, 2, 3, 4, 0, 0);
	btcheck_observe_vsh(PORT, &d);
	r.lx = 0x10;
	CHECK(btcheck_feed_raw(&r, 0, 2300000) == BTCHECK_FAILED);
	d = mk_vsh(5, 6, 7, 8, 0, 0);
	btcheck_observe_vsh(PORT, &d);
	CHECK(btcheck_vsh_samples_seen() == 2);
	/* only an explicit reset clears them */
	btcheck_reset(PORT);
	CHECK(btcheck_vsh_samples_seen() == 0);
	CHECK(btcheck_state() == BTCHECK_PENDING);
	return 0;
}

int run_tests(void)
{
	int r;
#define RUN(t) do { r = t(); if (r) return r; } while (0)
	RUN(test_decode_rejects_report_ids);
	RUN(test_decode_rejects_bad_hat);
	RUN(test_decode_fields);
	RUN(test_pass_after_50_matches);
	RUN(test_tolerance_2);
	RUN(test_buttons_must_match);
	RUN(test_ps_bit_ignored);
	RUN(test_fail_after_11_mismatches);
	RUN(test_mismatch_after_pass_ignored);
	RUN(test_history_window);
	RUN(test_repeated_raw_does_not_evict_history);
	RUN(test_unchanged_vsh_samples_not_counted);
	RUN(test_other_ports_and_empty_ignored);
	RUN(test_no_raw_yet_is_not_a_mismatch);
	RUN(test_freeze_after_disconnect);
	RUN(test_disconnect_while_pending_resets);
	RUN(test_vsh_comparison_gated_while_disconnected);
	RUN(test_vsh_counter_survives_failure);
	return 0;
}
