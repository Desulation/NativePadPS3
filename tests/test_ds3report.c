/* SOURCES: dualsense.c ds_body.c sixaxis.c util.c ../shim/ds3_report.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fakes.h"
#include "ds45pad.h"
#include "hook.h"
#include "sixaxis.h"
#include "../shim/shim_abi.h"
#include "../shim/ds3_report.h"

/* util.c is linked for ds45_crc32; the rest of it needs these two. */
ds45_pad_t g_pads[DS45_MAX_PADS];

void *ds45_resolve(const char *module, uint32_t fnid)
{
	(void)module; (void)fnid;
	return NULL;
}

#define RPT_LEN   78                   /* a whole Bluetooth report, CRC included */
#define BT(f)     (DS_OFF_BT + (f))    /* index of common block field f in it    */
#define BTN(n)    BT(DS_C_BUTTONS + (n))
#define SENTINEL  0xA5

static uint32_t g_rng;

static uint32_t rnd(void)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng >> 8;                  /* the low bits of an LCG are weak */
}

/* ------------------------------------------------------------------------ */
/* Synthetic reports and their expected DS3 translation                      */
/* ------------------------------------------------------------------------ */

/* Sticks centred, D-pad released (hat 8), everything else zero. */
static void report_neutral(uint8_t r[RPT_LEN])
{
	memset(r, 0, RPT_LEN);
	r[0] = 0x31;
	r[1] = 0x01;
	r[BT(DS_C_LX)] = r[BT(DS_C_LY)] = 0x80;
	r[BT(DS_C_RX)] = r[BT(DS_C_RY)] = 0x80;
	r[BTN(0)] = 0x08;
}

static void put_le16(uint8_t *p, int16_t v)
{
	p[0] = (uint8_t)((uint16_t)v & 0xFF);
	p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void set_accel(uint8_t *r, int axis, int16_t v)
{
	put_le16(r + BT(DS_C_ACCEL) + 2 * axis, v);
}

static void set_gyro(uint8_t *r, int axis, int16_t v)
{
	put_le16(r + BT(DS_C_GYRO) + 2 * axis, v);
}

/* The DS3 report for report_neutral() with motion on and no motion: only the
 * report ID, the sticks and the four 512 sensor words are non-zero. */
static void ds3_neutral(uint8_t e[DS3_REPORT_LEN])
{
	int i;

	memset(e, 0, DS3_REPORT_LEN);
	e[0] = 0x01;
	for (i = 6; i <= 9; i++)
		e[i] = 0x80;
	for (i = 0; i < 4; i++)
		e[41 + 2 * i] = 0x02;           /* 512 = 0x0200, big-endian */
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

static void set_sensor(uint8_t *e, int which, unsigned v)
{
	e[41 + 2 * which] = (uint8_t)(v >> 8);
	e[42 + 2 * which] = (uint8_t)(v & 0xFF);
}

/* Translate r and compare all 49 bytes with e, naming the first difference. */
static int translates_to(const uint8_t *r, const ds45_shim_cfg_t *c,
                         const uint8_t *e, const char *what)
{
	uint8_t out[DS3_REPORT_LEN];
	int i;

	memset(out, SENTINEL, sizeof(out));
	if (ds3_report_from_dualsense(r, RPT_LEN, c, out) != 0) {
		printf("  %s: rejected\n", what);
		return 0;
	}
	for (i = 0; i < DS3_REPORT_LEN; i++) {
		if (out[i] != e[i]) {
			printf("  %s: byte %d is 0x%02X, expected 0x%02X\n",
			       what, i, out[i], e[i]);
			return 0;
		}
	}
	return 1;
}

/* Bitwise CRC32 the way the DualSense computes it: the 0xA1 DATA|INPUT header
 * byte first, then the report up to the CRC field. Independent of util.c. */
static uint32_t ref_crc_input(const uint8_t *p, int n)
{
	uint32_t crc = 0xFFFFFFFFu;
	int i, b;

	for (i = -1; i < n; i++) {
		crc ^= (i < 0) ? 0xA1u : p[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return ~crc;
}

static void seal_crc(uint8_t r[RPT_LEN])
{
	uint32_t crc = ref_crc_input(r, 74);

	r[74] = (uint8_t)crc;
	r[75] = (uint8_t)(crc >> 8);
	r[76] = (uint8_t)(crc >> 16);
	r[77] = (uint8_t)(crc >> 24);
}

/* ------------------------------------------------------------------------ */
/* The translator                                                            */
/* ------------------------------------------------------------------------ */

static int test_neutral(void)
{
	uint8_t r[RPT_LEN], out[DS3_REPORT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	CHECK(DS3_REPORT_LEN == 49);

	report_neutral(r);
	memset(out, SENTINEL, sizeof(out));
	CHECK(ds3_report_from_dualsense(r, RPT_LEN, &c, out) == 0);
	CHECK(out[0] == 0x01);
	CHECK(out[2] == 0 && out[3] == 0 && out[4] == 0);
	CHECK(out[6] == 0x80 && out[7] == 0x80 && out[8] == 0x80 && out[9] == 0x80);
	CHECK(out[41] == 0x02 && out[42] == 0x00);       /* 512, big-endian */
	CHECK(out[43] == 0x02 && out[44] == 0x00);
	CHECK(out[45] == 0x02 && out[46] == 0x00);
	CHECK(out[47] == 0x02 && out[48] == 0x00);

	/* every other byte is 0 */
	ds3_neutral(want);
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* with motion off the sensors read the same neutral 512 */
	c.sixaxis = 0;
	CHECK(translates_to(r, &c, want, "neutral, sixaxis off"));
	return 0;
}

/* One row per DS3 bit: the DualSense control that drives it and where the DS3
 * puts the bit and its pressure byte. */
struct button_case {
	const char *name;
	uint8_t b0, b1, b2;      /* bits to set in DualSense button bytes 0..2    */
	int     hat;             /* D-pad direction 0..7, -1 for none             */
	uint8_t l2, r2;          /* analog trigger travel                         */
	int     out_byte;        /* DS3 digital byte                              */
	uint8_t out_mask;
	int     press;           /* DS3 pressure byte, 0 if the control has none  */
};

static const struct button_case k_buttons[] = {
	{ "Select",   0x00, 0x10, 0x00, -1,   0,   0, 2, 0x01,  0 },   /* Create  */
	{ "L3",       0x00, 0x40, 0x00, -1,   0,   0, 2, 0x02,  0 },
	{ "R3",       0x00, 0x80, 0x00, -1,   0,   0, 2, 0x04,  0 },
	{ "Start",    0x00, 0x20, 0x00, -1,   0,   0, 2, 0x08,  0 },   /* Options */
	{ "Up",       0x00, 0x00, 0x00,  0,   0,   0, 2, 0x10, 14 },
	{ "Right",    0x00, 0x00, 0x00,  2,   0,   0, 2, 0x20, 15 },
	{ "Down",     0x00, 0x00, 0x00,  4,   0,   0, 2, 0x40, 16 },
	{ "Left",     0x00, 0x00, 0x00,  6,   0,   0, 2, 0x80, 17 },
	{ "L2",       0x00, 0x04, 0x00, -1, 255,   0, 3, 0x01, 18 },
	{ "R2",       0x00, 0x08, 0x00, -1,   0, 255, 3, 0x02, 19 },
	{ "L1",       0x00, 0x01, 0x00, -1,   0,   0, 3, 0x04, 20 },
	{ "R1",       0x00, 0x02, 0x00, -1,   0,   0, 3, 0x08, 21 },
	{ "Triangle", 0x80, 0x00, 0x00, -1,   0,   0, 3, 0x10, 22 },
	{ "Circle",   0x40, 0x00, 0x00, -1,   0,   0, 3, 0x20, 23 },
	{ "Cross",    0x20, 0x00, 0x00, -1,   0,   0, 3, 0x40, 24 },
	{ "Square",   0x10, 0x00, 0x00, -1,   0,   0, 3, 0x80, 25 },
	{ "PS",       0x00, 0x00, 0x01, -1,   0,   0, 4, 0x01,  0 },
};

#define N_BUTTONS ((int)(sizeof(k_buttons) / sizeof(k_buttons[0])))

static void press_button(uint8_t *r, const struct button_case *k)
{
	r[BTN(0)] |= k->b0;
	if (k->hat >= 0)
		r[BTN(0)] = (uint8_t)((r[BTN(0)] & 0xF0) | k->hat);
	r[BTN(1)] |= k->b1;
	r[BTN(2)] |= k->b2;
	r[BT(DS_C_L2)] = k->l2;
	r[BT(DS_C_R2)] = k->r2;
}

static int test_buttons_each(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int i;

	CHECK(N_BUTTONS == 17);
	for (i = 0; i < N_BUTTONS; i++) {
		const struct button_case *k = &k_buttons[i];

		/* exactly this DS3 bit is set, its pressure byte reads 255, and
		 * nothing else in the report moves */
		report_neutral(r);
		press_button(r, k);
		ds3_neutral(want);
		want[k->out_byte] |= k->out_mask;
		if (k->press)
			want[k->press] = 255;
		CHECK(translates_to(r, &c, want, k->name));
	}
	return 0;
}

static int test_buttons_combined(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* Cross + Triangle, L1, R3, Options and PS with the D-pad on up-right */
	report_neutral(r);
	r[BTN(0)] = (uint8_t)(0x20 | 0x80 | 0x01);
	r[BTN(1)] = 0x01 | 0x80 | 0x20;
	r[BTN(2)] = 0x01;

	ds3_neutral(want);
	want[2] = 0x04 | 0x08 | 0x10 | 0x20;           /* R3 Start Up Right */
	want[3] = 0x04 | 0x10 | 0x40;                  /* L1 Triangle Cross */
	want[4] = 0x01;                                /* PS */
	want[14] = want[15] = 255;                      /* Up Right */
	want[20] = want[22] = want[24] = 255;            /* L1 Triangle Cross */
	CHECK(translates_to(r, &c, want, "combined"));
	return 0;
}

static int test_hat_directions(void)
{
	/* hat 0..7 run clockwise from up; 8 and above mean released */
	static const uint8_t k_dirs[8] = { 0x10, 0x30, 0x20, 0x60, 0x40, 0xC0, 0x80, 0x90 };
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int hat, b;

	for (hat = 0; hat < 16; hat++) {
		report_neutral(r);
		r[BTN(0)] = (uint8_t)hat;
		ds3_neutral(want);
		if (hat < 8) {
			want[2] = k_dirs[hat];
			for (b = 0; b < 4; b++)
				if (k_dirs[hat] & (0x10 << b))
					want[14 + b] = 255;
		}
		CHECK(translates_to(r, &c, want, "hat"));
	}
	return 0;
}

static int test_triggers_analog(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* The pressure bytes follow the analog travel, the digital bit follows
	 * the DualSense's own digital L2 flag. */
	report_neutral(r);
	r[BT(DS_C_L2)] = 0x40;
	r[BT(DS_C_R2)] = 0x7F;
	r[BTN(1)] = 0x04;                             /* digital L2 only */
	ds3_neutral(want);
	want[3] = 0x01;
	want[18] = 0x40;
	want[19] = 0x7F;
	CHECK(translates_to(r, &c, want, "l2 digital, r2 analog only"));

	/* travel without the digital flag: pressure, no bit */
	r[BTN(1)] = 0x00;
	want[3] = 0x00;
	CHECK(translates_to(r, &c, want, "analog only"));

	/* both flags, full travel */
	r[BTN(1)] = 0x04 | 0x08;
	r[BT(DS_C_L2)] = r[BT(DS_C_R2)] = 0xFF;
	want[3] = 0x03;
	want[18] = want[19] = 0xFF;
	CHECK(translates_to(r, &c, want, "both full"));
	return 0;
}

/* DualSense Edge extras have no DS3 equivalent and the shim has no mapping for
 * them: they must not leak into the report. */
static int test_edge_extras_ignored(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	report_neutral(r);
	r[BTN(2)] = 0xF0;
	r[BTN(3)] = 0xFF;
	ds3_neutral(want);
	CHECK(translates_to(r, &c, want, "edge extras"));
	return 0;
}

/* What each remap target does on the DS3 side. */
static const struct {
	int     out_byte;
	uint8_t out_mask;
	int     press;
} k_target[DS45_MAP_MAX] = {
	[DS45_MAP_NONE]     = { 0, 0x00,  0 },
	[DS45_MAP_CROSS]    = { 3, 0x40, 24 },
	[DS45_MAP_CIRCLE]   = { 3, 0x20, 23 },
	[DS45_MAP_SQUARE]   = { 3, 0x80, 25 },
	[DS45_MAP_TRIANGLE] = { 3, 0x10, 22 },
	[DS45_MAP_L1]       = { 3, 0x04, 20 },
	[DS45_MAP_R1]       = { 3, 0x08, 21 },
	[DS45_MAP_L2]       = { 3, 0x01, 18 },
	[DS45_MAP_R2]       = { 3, 0x02, 19 },
	[DS45_MAP_L3]       = { 2, 0x02,  0 },
	[DS45_MAP_R3]       = { 2, 0x04,  0 },
	[DS45_MAP_START]    = { 2, 0x08,  0 },
	[DS45_MAP_SELECT]   = { 2, 0x01,  0 },
	[DS45_MAP_PS]       = { 4, 0x01,  0 },
	[DS45_MAP_UP]       = { 2, 0x10, 14 },
	[DS45_MAP_RIGHT]    = { 2, 0x20, 15 },
	[DS45_MAP_DOWN]     = { 2, 0x40, 16 },
	[DS45_MAP_LEFT]     = { 2, 0x80, 17 },
};

static int test_map_touchpad_to_ps(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* touchpad click mapped to PS: the PS bit, nothing else */
	report_neutral(r);
	r[BTN(2)] = 0x02;
	c.map_touchpad = DS45_MAP_PS;
	ds3_neutral(want);
	want[4] = 0x01;
	CHECK(translates_to(r, &c, want, "touchpad -> PS"));

	/* the button must actually be down */
	r[BTN(2)] = 0x00;
	ds3_neutral(want);
	CHECK(translates_to(r, &c, want, "touchpad up"));

	/* and the Mute button is a different control with its own target */
	r[BTN(2)] = 0x04;
	CHECK(translates_to(r, &c, want, "mute with map_mute NONE"));
	c.map_mute = DS45_MAP_PS;
	want[4] = 0x01;
	CHECK(translates_to(r, &c, want, "mute -> PS"));

	/* NONE means nothing */
	report_neutral(r);
	r[BTN(2)] = 0x02 | 0x04;
	c.map_touchpad = c.map_mute = DS45_MAP_NONE;
	ds3_neutral(want);
	CHECK(translates_to(r, &c, want, "both NONE"));

	/* values that are not a ds45_map_t are ignored, not acted on */
	c.map_touchpad = DS45_MAP_MAX;
	c.map_mute = 200;
	CHECK(translates_to(r, &c, want, "out of range targets"));
	return 0;
}

static int test_map_every_target(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	int m;

	for (m = DS45_MAP_CROSS; m < DS45_MAP_MAX; m++) {
		ds45_shim_cfg_t c = cfg_plain();

		/* touchpad: the target's bit, and 255 on its pressure byte (the
		 * analog triggers are at rest, so the 255 is the mapping's) */
		report_neutral(r);
		r[BTN(2)] = 0x02;
		c.map_touchpad = (uint8_t)m;
		ds3_neutral(want);
		want[k_target[m].out_byte] |= k_target[m].out_mask;
		if (k_target[m].press)
			want[k_target[m].press] = 255;
		CHECK(translates_to(r, &c, want, "touchpad -> target"));

		/* mute: same table */
		report_neutral(r);
		r[BTN(2)] = 0x04;
		c.map_touchpad = DS45_MAP_NONE;
		c.map_mute = (uint8_t)m;
		CHECK(translates_to(r, &c, want, "mute -> target"));
	}
	return 0;
}

static int test_map_over_trigger(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* a mapped L2 reads full pressure even though the real trigger is only
	 * part way down */
	report_neutral(r);
	r[BTN(2)] = 0x02;
	r[BT(DS_C_L2)] = 0x10;
	c.map_touchpad = DS45_MAP_L2;
	ds3_neutral(want);
	want[3] = 0x01;
	want[18] = 255;
	CHECK(translates_to(r, &c, want, "touchpad -> L2"));
	return 0;
}

/* Stick bytes by their DualSense and DS3 positions. */
static const int k_stick_in[4]  = { DS_C_LX, DS_C_LY, DS_C_RX, DS_C_RY };
static const int k_stick_out[4] = { 6, 7, 8, 9 };

static int test_deadzone(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int a;

	for (a = 0; a < 4; a++) {
		/* 10 percent of the 128 steps of half travel is 12.8: twelve steps
		 * either side of centre are dead, thirteen are not */
		c.deadzone = 10;

		report_neutral(r);
		r[BT(k_stick_in[a])] = 0x80 + 12;
		ds3_neutral(want);
		CHECK(translates_to(r, &c, want, "centre + 12"));

		r[BT(k_stick_in[a])] = 0x80 + 13;
		want[k_stick_out[a]] = 0x80 + 13;
		CHECK(translates_to(r, &c, want, "centre + 13"));

		r[BT(k_stick_in[a])] = 0x80 - 12;
		want[k_stick_out[a]] = 0x80;
		CHECK(translates_to(r, &c, want, "centre - 12"));

		r[BT(k_stick_in[a])] = 0x80 - 13;
		want[k_stick_out[a]] = 0x80 - 13;
		CHECK(translates_to(r, &c, want, "centre - 13"));

		/* full throw is never touched */
		r[BT(k_stick_in[a])] = 0x00;
		want[k_stick_out[a]] = 0x00;
		CHECK(translates_to(r, &c, want, "full negative"));
		r[BT(k_stick_in[a])] = 0xFF;
		want[k_stick_out[a]] = 0xFF;
		CHECK(translates_to(r, &c, want, "full positive"));

		/* no deadzone: a one step offset comes through, either way */
		c.deadzone = 0;
		r[BT(k_stick_in[a])] = 0x81;
		want[k_stick_out[a]] = 0x81;
		CHECK(translates_to(r, &c, want, "dz 0, +1"));
		r[BT(k_stick_in[a])] = 0x7F;
		want[k_stick_out[a]] = 0x7F;
		CHECK(translates_to(r, &c, want, "dz 0, -1"));

		/* 25 percent is exactly 32 steps, and the edge itself is inside */
		c.deadzone = 25;
		r[BT(k_stick_in[a])] = 0x80 + 32;
		want[k_stick_out[a]] = 0x80;
		CHECK(translates_to(r, &c, want, "dz 25, +32"));
		r[BT(k_stick_in[a])] = 0x80 + 33;
		want[k_stick_out[a]] = 0x80 + 33;
		CHECK(translates_to(r, &c, want, "dz 25, +33"));

		/* at 100 percent even full throw is inside the zone */
		c.deadzone = 100;
		r[BT(k_stick_in[a])] = 0x00;
		want[k_stick_out[a]] = 0x80;
		CHECK(translates_to(r, &c, want, "dz 100, full negative"));
		r[BT(k_stick_in[a])] = 0xFF;
		CHECK(translates_to(r, &c, want, "dz 100, full positive"));
	}

	/* the axes are independent: one inside the zone, one outside */
	c.deadzone = 10;
	report_neutral(r);
	r[BT(DS_C_LX)] = 0x80 + 5;
	r[BT(DS_C_LY)] = 0x80 + 50;
	ds3_neutral(want);
	want[7] = 0x80 + 50;
	CHECK(translates_to(r, &c, want, "independent axes"));
	return 0;
}

static int test_motion_scale(void)
{
	uint8_t r[RPT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* 8192 counts is 1 g: 113 steps above the 512 rest value, 0x0271 */
	report_neutral(r);
	set_accel(r, 0, 8192);
	CHECK(r[BT(DS_C_ACCEL)] == 0x00 && r[BT(DS_C_ACCEL) + 1] == 0x20);
	ds3_neutral(want);
	want[41] = 0x02;
	want[42] = 0x71;
	CHECK(translates_to(r, &c, want, "accel x = 1 g"));

	/* each axis lands in its own word; negative counts go below the rest value */
	set_accel(r, 1, -8192);
	set_accel(r, 2, 8192);
	set_gyro(r, 1, 3200);
	set_sensor(want, 1, 399);
	set_sensor(want, 2, 625);
	set_sensor(want, 3, 612);
	CHECK(translates_to(r, &c, want, "all four"));

	/* only the yaw gyro feeds the single DS3 gyro word */
	report_neutral(r);
	set_gyro(r, 0, 10000);
	set_gyro(r, 2, -10000);
	ds3_neutral(want);
	CHECK(translates_to(r, &c, want, "pitch and roll ignored"));

	/* scale is a percentage of the physical value */
	report_neutral(r);
	set_accel(r, 0, 8192);
	set_gyro(r, 1, 3200);
	c.accel_scale = 200;
	c.gyro_scale = 50;
	ds3_neutral(want);
	set_sensor(want, 0, 512 + 226);
	set_sensor(want, 3, 512 + 50);
	CHECK(translates_to(r, &c, want, "scaled"));

	/* the inversions are per output axis */
	c = cfg_plain();
	c.invert_x = 1;
	c.invert_g = 1;
	ds3_neutral(want);
	set_sensor(want, 0, 512 - 113);
	set_sensor(want, 3, 512 - 100);
	CHECK(translates_to(r, &c, want, "invert x and g"));

	report_neutral(r);
	set_accel(r, 1, -8192);
	set_accel(r, 2, 8192);
	c = cfg_plain();
	c.invert_y = 1;
	c.invert_z = 1;
	ds3_neutral(want);
	set_sensor(want, 1, 512 + 113);
	set_sensor(want, 2, 512 - 113);
	CHECK(translates_to(r, &c, want, "invert y and z"));

	/* swap_xy exchanges the two source axes before the inversion */
	report_neutral(r);
	set_accel(r, 0, 8192);
	set_accel(r, 1, -8192);
	c = cfg_plain();
	c.swap_xy = 1;
	ds3_neutral(want);
	set_sensor(want, 0, 512 - 113);
	set_sensor(want, 1, 512 + 113);
	CHECK(translates_to(r, &c, want, "swap"));
	c.invert_x = 1;
	set_sensor(want, 0, 512 + 113);
	CHECK(translates_to(r, &c, want, "swap and invert x"));

	/* the 10 bit range is clamped, not wrapped */
	report_neutral(r);
	set_accel(r, 0, 32767);
	set_accel(r, 1, -32768);
	set_gyro(r, 1, 32767);
	c = cfg_plain();
	c.accel_scale = 300;
	ds3_neutral(want);
	set_sensor(want, 0, 1023);
	set_sensor(want, 1, 0);
	set_sensor(want, 3, 1023);
	CHECK(translates_to(r, &c, want, "clamped"));

	/* with motion off the raw values are ignored altogether */
	c.sixaxis = 0;
	ds3_neutral(want);
	CHECK(translates_to(r, &c, want, "sixaxis off"));
	return 0;
}

static int test_sixaxis_values(void)
{
	/* accel: 8192 counts per g in, 113 steps per g out, truncating toward
	 * zero, then the percentage scale, then the optional inversion */
	CHECK(DS45_SENSOR_CENTER == 512);
	CHECK(sixaxis_from_accel(0, 100, 0) == 512);
	CHECK(sixaxis_from_accel(8192, 100, 0) == 625);
	CHECK(sixaxis_from_accel(-8192, 100, 0) == 399);
	CHECK(sixaxis_from_accel(100, 100, 0) == 513);
	CHECK(sixaxis_from_accel(-100, 100, 0) == 511);
	CHECK(sixaxis_from_accel(8192, 200, 0) == 738);
	CHECK(sixaxis_from_accel(8192, 100, 1) == 399);
	CHECK(sixaxis_from_accel(-8192, 100, 1) == 625);
	CHECK(sixaxis_from_accel(8192, 0, 0) == 512);
	CHECK(sixaxis_from_accel(32767, 100, 0) == 963);
	CHECK(sixaxis_from_accel(-32768, 100, 0) == 60);
	CHECK(sixaxis_from_accel(32767, 300, 0) == 1023);
	CHECK(sixaxis_from_accel(-32768, 300, 0) == 0);

	/* gyro: divided by 32, then scaled and inverted the same way */
	CHECK(sixaxis_from_gyro(0, 100, 0) == 512);
	CHECK(sixaxis_from_gyro(3200, 100, 0) == 612);
	CHECK(sixaxis_from_gyro(-3200, 100, 0) == 412);
	CHECK(sixaxis_from_gyro(3200, 100, 1) == 412);
	CHECK(sixaxis_from_gyro(3200, 50, 0) == 562);
	CHECK(sixaxis_from_gyro(63, 100, 0) == 513);
	CHECK(sixaxis_from_gyro(31, 100, 0) == 512);
	CHECK(sixaxis_from_gyro(-31, 100, 0) == 512);
	CHECK(sixaxis_from_gyro(-32, 100, 0) == 511);
	CHECK(sixaxis_from_gyro(32767, 100, 0) == 1023);
	CHECK(sixaxis_from_gyro(-32768, 100, 0) == 0);
	return 0;
}

static int test_rejects_wrong_id_and_short(void)
{
	uint8_t r[128], out[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int id, len, i;

	/* the shortest report that holds the status byte, the last field read */
	CHECK(DS_BT_BODY_MIN == 55);
	CHECK(DS_BT_BODY_MIN == DS_OFF_BT + DS_C_STATUS + 1);

	memset(r, 0, sizeof(r));
	report_neutral(r);

	for (id = 0; id < 256; id++) {
		if (id == 0x31)
			continue;
		r[0] = (uint8_t)id;
		memset(out, SENTINEL, sizeof(out));
		CHECK(ds3_report_from_dualsense(r, RPT_LEN, &c, out) == -1);
		for (i = 0; i < DS3_REPORT_LEN; i++)
			CHECK(out[i] == SENTINEL);
	}
	r[0] = 0x31;

	for (len = -3; len < DS_BT_BODY_MIN; len++) {
		memset(out, SENTINEL, sizeof(out));
		CHECK(ds3_report_from_dualsense(r, len, &c, out) == -1);
		for (i = 0; i < DS3_REPORT_LEN; i++)
			CHECK(out[i] == SENTINEL);
	}

	/* at the minimum, and at the lengths the shim really sees (the kernel
	 * buffer cuts a 78 byte report to 64) and beyond, it is accepted */
	CHECK(ds3_report_from_dualsense(r, DS_BT_BODY_MIN, &c, out) == 0);
	CHECK(ds3_report_from_dualsense(r, 64, &c, out) == 0);
	CHECK(ds3_report_from_dualsense(r, RPT_LEN, &c, out) == 0);
	CHECK(ds3_report_from_dualsense(r, 128, &c, out) == 0);
	CHECK(out[0] == 0x01);

	/* missing arguments are a refusal too, and leave the output alone */
	memset(out, SENTINEL, sizeof(out));
	CHECK(ds3_report_from_dualsense(NULL, RPT_LEN, &c, out) == -1);
	CHECK(ds3_report_from_dualsense(r, RPT_LEN, NULL, out) == -1);
	CHECK(ds3_report_from_dualsense(r, RPT_LEN, &c, NULL) == -1);
	for (i = 0; i < DS3_REPORT_LEN; i++)
		CHECK(out[i] == SENTINEL);
	return 0;
}

/* The shim translates in place: the report it reads and the 49 bytes it writes
 * are the same buffer. Everything must be read before the first byte is written. */
static int test_in_place(void)
{
	uint8_t src[128], buf[128], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int n, i;

	g_rng = 0x5EED0001u;
	for (n = 0; n < 500; n++) {
		for (i = 0; i < 64; i++)
			src[i] = (uint8_t)rnd();
		src[0] = 0x31;
		c.deadzone = (uint8_t)(rnd() % 30);
		c.swap_xy = (uint8_t)(rnd() & 1);
		c.map_touchpad = (uint8_t)(rnd() % DS45_MAP_MAX);
		c.map_mute = (uint8_t)(rnd() % DS45_MAP_MAX);

		CHECK(ds3_report_from_dualsense(src, 64, &c, want) == 0);
		memcpy(buf, src, 64);
		CHECK(ds3_report_from_dualsense(buf, 64, &c, buf) == 0);
		CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0);
	}
	return 0;
}

/* Output must depend on the first len bytes and nothing after them. */
static int test_no_read_past_len(void)
{
	uint8_t tight[DS_BT_BODY_MIN], full[128], a[DS3_REPORT_LEN], b[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int i;

	g_rng = 0xABCDEF01u;
	for (i = 0; i < DS_BT_BODY_MIN; i++)
		tight[i] = (uint8_t)rnd();
	tight[0] = 0x31;
	memcpy(full, tight, sizeof(tight));
	memset(full + DS_BT_BODY_MIN, 0xFF, sizeof(full) - DS_BT_BODY_MIN);

	CHECK(ds3_report_from_dualsense(tight, DS_BT_BODY_MIN, &c, a) == 0);
	CHECK(ds3_report_from_dualsense(full, DS_BT_BODY_MIN, &c, b) == 0);
	CHECK(memcmp(a, b, sizeof(a)) == 0);
	memset(full + DS_BT_BODY_MIN, 0x00, sizeof(full) - DS_BT_BODY_MIN);
	CHECK(ds3_report_from_dualsense(full, DS_BT_BODY_MIN, &c, b) == 0);
	CHECK(memcmp(a, b, sizeof(a)) == 0);
	return 0;
}

static int layout_byte(int i)
{
	return i == 0 || i == 2 || i == 3 || i == 4 || (i >= 6 && i <= 9) ||
	       (i >= 14 && i <= 25) || (i >= 41 && i <= 48);
}

static int test_fuzz(void)
{
	uint8_t in[128], in2[128], out[DS3_REPORT_LEN], out2[DS3_REPORT_LEN];
	int n, i, len, ok;

	g_rng = 0xD5C0FFEEu;
	for (n = 0; n < 10000; n++) {
		ds45_shim_cfg_t c;

		len = (int)(rnd() % 129);                  /* 0..128 */
		for (i = 0; i < 128; i++)
			in[i] = (uint8_t)rnd();
		if (rnd() & 1)
			in[0] = 0x31;                      /* half the time a plausible ID */

		c.deadzone = (uint8_t)rnd();
		c.map_touchpad = (uint8_t)(rnd() % 24);    /* some are not valid targets */
		c.map_mute = (uint8_t)(rnd() % 24);
		c.sixaxis = (uint8_t)(rnd() % 3);
		c.swap_xy = (uint8_t)(rnd() % 3);
		c.invert_x = (uint8_t)(rnd() % 3);
		c.invert_y = (uint8_t)(rnd() % 3);
		c.invert_z = (uint8_t)(rnd() % 3);
		c.invert_g = (uint8_t)(rnd() % 3);
		c.accel_scale = (int16_t)rnd();
		c.gyro_scale = (int16_t)rnd();

		ok = len >= DS_BT_BODY_MIN && in[0] == 0x31;
		memset(out, SENTINEL, sizeof(out));
		if (!ok) {
			CHECK(ds3_report_from_dualsense(in, len, &c, out) == -1);
			for (i = 0; i < DS3_REPORT_LEN; i++)
				CHECK(out[i] == SENTINEL);
			continue;
		}
		CHECK(ds3_report_from_dualsense(in, len, &c, out) == 0);

		/* the shape of the report */
		CHECK(out[0] == 0x01);
		for (i = 0; i < DS3_REPORT_LEN; i++)
			if (!layout_byte(i))
				CHECK(out[i] == 0);
		/* every pressure byte but the triggers' is 0 or 255, and it is 255
		 * exactly when its digital bit is set */
		for (i = 14; i <= 25; i++)
			if (i != 18 && i != 19)
				CHECK(out[i] == 0 || out[i] == 255);
		for (i = 0; i < 4; i++)
			CHECK(((out[2] & (0x10 << i)) != 0) == (out[14 + i] == 255));
		for (i = 0; i < 6; i++)
			CHECK(((out[3] & (0x04 << i)) != 0) == (out[20 + i] == 255));
		for (i = 0; i < 4; i++) {
			unsigned w = ((unsigned)out[41 + 2 * i] << 8) | out[42 + 2 * i];
			CHECK(w <= 1023);
			if (!c.sixaxis)
				CHECK(w == 512);
		}

		/* sticks: untouched, or snapped to centre inside the deadzone */
		for (i = 0; i < 4; i++) {
			int v = in[BT(k_stick_in[i])];
			int d = v < 0x80 ? 0x80 - v : v - 0x80;
			int want = (d * 100 <= c.deadzone * 128) ? 0x80 : v;

			CHECK(out[k_stick_out[i]] == want);
		}

		/* nothing past len was read */
		memcpy(in2, in, sizeof(in2));
		for (i = len; i < 128; i++)
			in2[i] = (uint8_t)~in[i];
		CHECK(ds3_report_from_dualsense(in2, len, &c, out2) == 0);
		CHECK(memcmp(out, out2, DS3_REPORT_LEN) == 0);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The basic report 0x01 a DualSense sends before it is switched to 0x31      */
/* ------------------------------------------------------------------------ */

#define BASIC_GUARD 32                 /* guard bytes after the input in the fuzz */

/* Sticks centred, hat released, no buttons, triggers at rest. */
static void basic_neutral(uint8_t r[DS_BASIC_LEN])
{
	static const uint8_t k[DS_BASIC_LEN] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0, 0, 0, 0 };

	memcpy(r, k, DS_BASIC_LEN);
}

/* Translate the basic report r and compare all 49 bytes with e. */
static int basic_translates_to(const uint8_t *r, const ds45_shim_cfg_t *c,
                               const uint8_t *e, const char *what)
{
	uint8_t out[DS3_REPORT_LEN];
	int i;

	memset(out, SENTINEL, sizeof(out));
	if (ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, c, out) != 0) {
		printf("  %s: rejected\n", what);
		return 0;
	}
	for (i = 0; i < DS3_REPORT_LEN; i++) {
		if (out[i] != e[i]) {
			printf("  %s: byte %d is 0x%02X, expected 0x%02X\n",
			       what, i, out[i], e[i]);
			return 0;
		}
	}
	return 1;
}

static int test_basic_neutral(void)
{
	uint8_t r[DS_BASIC_LEN], out[DS3_REPORT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int i;

	CHECK(DS_BASIC_REPORT_ID == 0x01);
	CHECK(DS_BASIC_LEN == 10);

	basic_neutral(r);
	memset(out, SENTINEL, sizeof(out));
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[0] == 0x01);
	for (i = 6; i <= 9; i++)
		CHECK(out[i] == 0x80);
	CHECK(out[2] == 0 && out[3] == 0 && out[4] == 0);
	for (i = 0; i < 4; i++)                       /* 02 00 02 00 02 00 02 00 */
		CHECK(out[41 + 2 * i] == 0x02 && out[42 + 2 * i] == 0x00);

	/* every other byte is 0: the same report the 0x31 path gives at rest */
	ds3_neutral(want);
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* the basic report has no motion data: centre whatever cfg->sixaxis says */
	c.sixaxis = 0;
	CHECK(basic_translates_to(r, &c, want, "basic neutral, sixaxis off"));

	/* the sticks pass through, and the deadzone applies to them */
	c = cfg_plain();
	r[1] = 0x10; r[2] = 0x20; r[3] = 0xE0; r[4] = 0xF0;
	want[6] = 0x10; want[7] = 0x20; want[8] = 0xE0; want[9] = 0xF0;
	CHECK(basic_translates_to(r, &c, want, "basic sticks"));
	c.deadzone = 10;
	r[1] = 0x80 + 12;                             /* inside: centred      */
	r[4] = 0x80 - 13;                             /* outside: passes      */
	want[6] = 0x80;
	want[9] = 0x80 - 13;
	CHECK(basic_translates_to(r, &c, want, "basic deadzone"));
	return 0;
}

static int test_basic_buttons(void)
{
	uint8_t r[DS_BASIC_LEN], out[DS3_REPORT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* Cross: its bit and its full pressure byte, nothing else */
	basic_neutral(r);
	r[5] = 0x28;
	ds3_neutral(want);
	want[3] = 0x40;
	want[24] = 255;
	CHECK(basic_translates_to(r, &c, want, "basic Cross"));

	/* Options is Start */
	basic_neutral(r);
	r[6] = 0x20;
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[2] & 0x08);
	ds3_neutral(want);
	want[2] = 0x08;
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* PS */
	basic_neutral(r);
	r[7] = 0x01;
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[4] & 0x01);
	ds3_neutral(want);
	want[4] = 0x01;
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* hat 2 is Right, with its pressure byte */
	basic_neutral(r);
	r[5] = 0x02;
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[2] & 0x20);
	ds3_neutral(want);
	want[2] = 0x20;
	want[15] = 255;
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* Create is Select; the rest of byte 6 and the face buttons */
	basic_neutral(r);
	r[5] = 0x08 | 0x10 | 0x40 | 0x80;            /* Square Circle Triangle */
	r[6] = 0x01 | 0x02 | 0x10 | 0x40 | 0x80;     /* L1 R1 Create L3 R3 */
	ds3_neutral(want);
	want[2] = 0x01 | 0x02 | 0x04;                 /* Select L3 R3 */
	want[3] = 0x04 | 0x08 | 0x10 | 0x20 | 0x80;   /* L1 R1 Triangle Circle Square */
	want[20] = want[21] = want[22] = want[23] = want[25] = 255;
	CHECK(basic_translates_to(r, &c, want, "basic face and shoulders"));

	/* touchpad click follows cfg->map_touchpad */
	basic_neutral(r);
	r[7] = 0x02;
	ds3_neutral(want);
	CHECK(basic_translates_to(r, &c, want, "basic touchpad, map NONE"));
	c.map_touchpad = DS45_MAP_PS;
	want[4] = 0x01;
	CHECK(basic_translates_to(r, &c, want, "basic touchpad -> PS"));

	/* the rest of byte 7 is not a button (a DS4-style counter): it must not
	 * reach the Mute mapping or anything else */
	c = cfg_plain();
	c.map_mute = DS45_MAP_PS;
	basic_neutral(r);
	r[7] = 0xFC;
	ds3_neutral(want);
	CHECK(basic_translates_to(r, &c, want, "basic byte 7 upper bits"));
	return 0;
}

static int test_basic_triggers(void)
{
	uint8_t r[DS_BASIC_LEN], out[DS3_REPORT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();

	/* analog travel only: the pressure bytes, no digital bits */
	basic_neutral(r);
	r[8] = 0x40;
	r[9] = 0xFF;
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[18] == 0x40);
	CHECK(out[19] == 0xFF);
	ds3_neutral(want);
	want[18] = 0x40;
	want[19] = 0xFF;
	CHECK(memcmp(out, want, sizeof(want)) == 0);

	/* the digital bits follow the report's own L2/R2 flags */
	r[6] = 0x04 | 0x08;
	want[3] = 0x01 | 0x02;
	CHECK(basic_translates_to(r, &c, want, "basic L2 R2 digital"));
	return 0;
}

static int test_basic_refuses(void)
{
	uint8_t r[DS_BASIC_LEN], out[DS3_REPORT_LEN], ee[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int id, len;

	memset(ee, 0xEE, sizeof(ee));
	basic_neutral(r);

	memset(out, 0xEE, sizeof(out));
	CHECK(ds3_report_from_dualsense_basic(NULL, DS_BASIC_LEN, &c, out) == -1);
	CHECK(memcmp(out, ee, sizeof(out)) == 0);
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, NULL, out) == -1);
	CHECK(memcmp(out, ee, sizeof(out)) == 0);
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, NULL) == -1);

	r[0] = 0x31;
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == -1);
	CHECK(memcmp(out, ee, sizeof(out)) == 0);
	for (id = 0; id < 256; id++) {
		if (id == DS_BASIC_REPORT_ID)
			continue;
		r[0] = (uint8_t)id;
		CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == -1);
		CHECK(memcmp(out, ee, sizeof(out)) == 0);
	}
	r[0] = DS_BASIC_REPORT_ID;

	CHECK(ds3_report_from_dualsense_basic(r, 9, &c, out) == -1);
	CHECK(memcmp(out, ee, sizeof(out)) == 0);
	for (len = -3; len < DS_BASIC_LEN; len++) {
		CHECK(ds3_report_from_dualsense_basic(r, len, &c, out) == -1);
		CHECK(memcmp(out, ee, sizeof(out)) == 0);
	}

	/* and at the length it is accepted */
	CHECK(ds3_report_from_dualsense_basic(r, DS_BASIC_LEN, &c, out) == 0);
	CHECK(out[0] == 0x01);
	return 0;
}

/* The shim will translate the basic report in place too. */
static int test_basic_in_place(void)
{
	uint8_t src[DS3_REPORT_LEN], buf[DS3_REPORT_LEN], want[DS3_REPORT_LEN];
	ds45_shim_cfg_t c = cfg_plain();
	int n, i;

	g_rng = 0xBA51C001u;
	for (n = 0; n < 500; n++) {
		for (i = 0; i < DS3_REPORT_LEN; i++)
			src[i] = (uint8_t)rnd();
		src[0] = DS_BASIC_REPORT_ID;
		c.deadzone = (uint8_t)(rnd() % 30);
		c.map_touchpad = (uint8_t)(rnd() % DS45_MAP_MAX);
		c.map_mute = (uint8_t)(rnd() % DS45_MAP_MAX);

		CHECK(ds3_report_from_dualsense_basic(src, DS3_REPORT_LEN, &c, want) == 0);
		memcpy(buf, src, sizeof(buf));
		CHECK(ds3_report_from_dualsense_basic(buf, DS3_REPORT_LEN, &c, buf) == 0);
		CHECK(memcmp(buf, want, DS3_REPORT_LEN) == 0);
	}
	return 0;
}

static int test_basic_fuzz_all_lengths(void)
{
	/* the input sits at the end of area[0..0x41), the guard bytes follow it */
	uint8_t area[0x41 + BASIC_GUARD], out[DS3_REPORT_LEN], out2[DS3_REPORT_LEN];
	int len, n, i, ok, r1, r2;

	g_rng = 0xF0221001u;
	for (len = 0; len <= 0x41; len++) {
		uint8_t *in = area + 0x41 - len;

		for (n = 0; n < 1000; n++) {
			ds45_shim_cfg_t c;

			for (i = 0; i < (int)sizeof(area); i++)
				area[i] = (uint8_t)rnd();
			if (len > 0 && (rnd() & 1))
				in[0] = DS_BASIC_REPORT_ID;    /* half the time a plausible ID */

			c.deadzone = (uint8_t)rnd();
			c.map_touchpad = (uint8_t)(rnd() % 24);
			c.map_mute = (uint8_t)(rnd() % 24);
			c.sixaxis = (uint8_t)(rnd() % 3);
			c.swap_xy = (uint8_t)(rnd() % 3);
			c.invert_x = (uint8_t)(rnd() % 3);
			c.invert_y = (uint8_t)(rnd() % 3);
			c.invert_z = (uint8_t)(rnd() % 3);
			c.invert_g = (uint8_t)(rnd() % 3);
			c.accel_scale = (int16_t)rnd();
			c.gyro_scale = (int16_t)rnd();

			ok = len >= DS_BASIC_LEN && in[0] == DS_BASIC_REPORT_ID;

			/* guard pattern one */
			memset(area + 0x41, 0x00, BASIC_GUARD);
			memset(out, SENTINEL, sizeof(out));
			r1 = ds3_report_from_dualsense_basic(in, len, &c, out);
			CHECK(r1 == (ok ? 0 : -1));

			/* guard pattern two, and every byte before the input changed
			 * too: the output must not move */
			memset(area + 0x41, 0xFF, BASIC_GUARD);
			for (i = 0; i < 0x41 - len; i++)
				area[i] = (uint8_t)~area[i];
			memset(out2, SENTINEL, sizeof(out2));
			r2 = ds3_report_from_dualsense_basic(in, len, &c, out2);
			CHECK(r2 == r1);
			CHECK(memcmp(out, out2, DS3_REPORT_LEN) == 0);

			if (!ok) {
				for (i = 0; i < DS3_REPORT_LEN; i++)
					CHECK(out[i] == SENTINEL);
				continue;
			}

			/* the shape of the report, as for 0x31 */
			CHECK(out[0] == 0x01);
			for (i = 0; i < DS3_REPORT_LEN; i++)
				if (!layout_byte(i))
					CHECK(out[i] == 0);
			for (i = 0; i < 4; i++)
				CHECK(out[41 + 2 * i] == 0x02 && out[42 + 2 * i] == 0x00);
			for (i = 0; i < 4; i++) {
				int v = in[1 + i];
				int d = v < 0x80 ? 0x80 - v : v - 0x80;
				int want = (d * 100 <= c.deadzone * 128) ? 0x80 : v;

				CHECK(out[6 + i] == want);
			}
		}
	}
	return 0;
}

static int test_abi_layout(void)
{
	int i;

	/* "DS45SHM1", most significant byte first */
	for (i = 0; i < 8; i++)
		CHECK((uint8_t)(DS45_SHIM_MAGIC >> (56 - 8 * i)) == (uint8_t)"DS45SHM1"[i]);
	CHECK(DS45_SHIM_VERSION == 3);     /* 3: start-up completion, no connection table */
	CHECK(DS45_SHIM_INFO_LEN == 0x56);

	/* The plugin and the blob compile this header separately and meet in
	 * memory, so these sizes and offsets are the contract (fixed-width fields
	 * only, at their natural alignment). They were also checked against the
	 * SDK's ppu-lv2-gcc with both the plugin's and the blob's flags. */
	CHECK(sizeof(ds45_shim_cfg_t) == 14);
	CHECK(offsetof(ds45_shim_cfg_t, deadzone) == 0);
	CHECK(offsetof(ds45_shim_cfg_t, map_touchpad) == 1);
	CHECK(offsetof(ds45_shim_cfg_t, map_mute) == 2);
	CHECK(offsetof(ds45_shim_cfg_t, sixaxis) == 3);
	CHECK(offsetof(ds45_shim_cfg_t, invert_g) == 8);
	CHECK(offsetof(ds45_shim_cfg_t, accel_scale) == 10);
	CHECK(offsetof(ds45_shim_cfg_t, gyro_scale) == 12);

	CHECK(offsetof(ds45_shim_data_t, magic) == 0);
	CHECK(offsetof(ds45_shim_data_t, version) == 8);
	CHECK(offsetof(ds45_shim_data_t, enabled) == 12);
	CHECK(offsetof(ds45_shim_data_t, orig_event) == 16);
	CHECK(offsetof(ds45_shim_data_t, orig_enable) == 24);
	CHECK(offsetof(ds45_shim_data_t, orig_input) == 32);
	CHECK(offsetof(ds45_shim_data_t, pad_vtable) == 40);
	CHECK(offsetof(ds45_shim_data_t, pad_mgr) == 48);
	CHECK(offsetof(ds45_shim_data_t, cfg) == 56);
	CHECK(offsetof(ds45_shim_data_t, stat_completed) == 72);
	CHECK(offsetof(ds45_shim_data_t, stat_enabled) == 76);
	CHECK(offsetof(ds45_shim_data_t, stat_translated) == 80);
	CHECK(offsetof(ds45_shim_data_t, stat_passed) == 84);
	CHECK(offsetof(ds45_shim_data_t, info_tmpl) == 88);
	CHECK(sizeof(((ds45_shim_data_t *)0)->info_tmpl) == 0x58);
	/* appended after info_tmpl: the product-ID sites' original qwords and
	 * which of them the install applied (adoption rebuilds its journal) */
	CHECK(DS45_SHIM_PID_SITES == 3);
	CHECK(offsetof(ds45_shim_data_t, pid_orig) == 176);
	CHECK(sizeof(((ds45_shim_data_t *)0)->pid_orig) == 8 * DS45_SHIM_PID_SITES);
	CHECK(offsetof(ds45_shim_data_t, pid_applied) == 200);
	CHECK(sizeof(ds45_shim_data_t) == 208);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* The parser underneath: dualsense_parse() after the split                  */
/* ------------------------------------------------------------------------ */

static int same_state(const ds45_state_t *a, const ds45_state_t *b)
{
	int i;

	if (a->lx != b->lx || a->ly != b->ly || a->rx != b->rx || a->ry != b->ry ||
	    a->l2 != b->l2 || a->r2 != b->r2 || a->buttons != b->buttons ||
	    a->battery != b->battery || a->charging != b->charging ||
	    a->full != b->full || a->valid != b->valid ||
	    a->sticks_valid != b->sticks_valid)
		return 0;
	for (i = 0; i < 3; i++)
		if (a->gyro[i] != b->gyro[i] || a->accel[i] != b->accel[i])
			return 0;
	for (i = 0; i < 2; i++)
		if (a->touch[i].x != b->touch[i].x || a->touch[i].y != b->touch[i].y ||
		    a->touch[i].id != b->touch[i].id ||
		    a->touch[i].active != b->touch[i].active)
			return 0;
	return 1;
}

/* A 63 byte common block with a distinct value in every field, hand-decoded in
 * test_parse_fields(). */
static void common_known(uint8_t c[DS_C_SIZE])
{
	memset(c, 0, DS_C_SIZE);
	c[DS_C_LX] = 0x12; c[DS_C_LY] = 0x34; c[DS_C_RX] = 0x56; c[DS_C_RY] = 0x78;
	c[DS_C_L2] = 0x9A; c[DS_C_R2] = 0xBC;
	c[DS_C_SEQ] = 0x07;
	c[DS_C_BUTTONS + 0] = 0xF3;                 /* hat 3 + the four face buttons */
	c[DS_C_BUTTONS + 1] = 0xFF;                 /* shoulders, Create, Options, sticks */
	c[DS_C_BUTTONS + 2] = 0x07;                 /* PS, touchpad, Mute */
	c[DS_C_BUTTONS + 3] = 0xFF;                 /* counter, ignored */
	c[DS_C_GYRO + 0] = 0xFE; c[DS_C_GYRO + 1] = 0xFF;     /* -2 */
	c[DS_C_GYRO + 2] = 0x34; c[DS_C_GYRO + 3] = 0x12;     /* 0x1234 */
	c[DS_C_GYRO + 4] = 0x00; c[DS_C_GYRO + 5] = 0x80;     /* -32768 */
	c[DS_C_ACCEL + 0] = 0x00; c[DS_C_ACCEL + 1] = 0x20;   /* 8192 */
	c[DS_C_ACCEL + 2] = 0xFF; c[DS_C_ACCEL + 3] = 0xFF;   /* -1 */
	c[DS_C_ACCEL + 4] = 0xFF; c[DS_C_ACCEL + 5] = 0x7F;   /* 32767 */
	c[DS_C_TOUCH + 0] = 0x05; c[DS_C_TOUCH + 1] = 0x34;   /* finger 5 down */
	c[DS_C_TOUCH + 2] = 0x92; c[DS_C_TOUCH + 3] = 0x56;
	c[DS_C_TOUCH + 4] = 0x85; c[DS_C_TOUCH + 5] = 0xFF;   /* finger 5 up */
	c[DS_C_TOUCH + 6] = 0xFF; c[DS_C_TOUCH + 7] = 0xFF;
	c[DS_C_STATUS] = 0x13;                      /* battery 3, charging */
}

static void place_usb(uint8_t u[64], const uint8_t c[DS_C_SIZE])
{
	memset(u, 0, 64);
	u[0] = 0x01;
	memcpy(u + 1, c, DS_C_SIZE);
}

static void place_bt(uint8_t b[RPT_LEN], const uint8_t c[DS_C_SIZE])
{
	memset(b, 0, RPT_LEN);
	b[0] = 0x31;
	b[1] = 0x01;
	memcpy(b + 2, c, DS_C_SIZE);
	seal_crc(b);
}

static int check_known(const ds45_state_t *st)
{
	return st->valid == 1 && st->sticks_valid == 1 &&
	       st->lx == 0x12 && st->ly == 0x34 && st->rx == 0x56 && st->ry == 0x78 &&
	       st->l2 == 0x9A && st->r2 == 0xBC &&
	       st->buttons == (DS45_B_RIGHT | DS45_B_DOWN |
	                       DS45_B_SQUARE | DS45_B_CROSS | DS45_B_CIRCLE | DS45_B_TRIANGLE |
	                       DS45_B_L1 | DS45_B_R1 | DS45_B_L2 | DS45_B_R2 |
	                       DS45_B_SHARE | DS45_B_OPTIONS | DS45_B_L3 | DS45_B_R3 |
	                       DS45_B_PS | DS45_B_TOUCHPAD | DS45_B_MUTE) &&
	       st->gyro[0] == -2 && st->gyro[1] == 0x1234 && st->gyro[2] == -32768 &&
	       st->accel[0] == 8192 && st->accel[1] == -1 && st->accel[2] == 32767 &&
	       st->touch[0].active == 1 && st->touch[0].id == 5 &&
	       st->touch[0].x == 0x234 && st->touch[0].y == 0x569 &&
	       st->touch[1].active == 0 && st->touch[1].id == 5 &&
	       st->touch[1].x == 0xFFF && st->touch[1].y == 0xFFF &&
	       st->battery == 3 && st->charging == 1 && st->full == 0;
}

static int test_parse_fields(void)
{
	uint8_t c[DS_C_SIZE], u[64], b[RPT_LEN];
	ds45_state_t st;

	common_known(c);
	place_usb(u, c);
	place_bt(b, c);

	/* the same block decodes the same through all three entry points */
	memset(&st, 0xEE, sizeof(st));
	CHECK(dualsense_parse(u, 64, DS45_LINK_USB, &st) == 0);
	CHECK(check_known(&st));

	memset(&st, 0xEE, sizeof(st));
	CHECK(dualsense_parse(b, RPT_LEN, DS45_LINK_BT, &st) == 0);
	CHECK(check_known(&st));

	memset(&st, 0xEE, sizeof(st));
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK(check_known(&st));

	/* the body parser needs no CRC and no tail: just through the status byte */
	memset(&st, 0xEE, sizeof(st));
	CHECK(dualsense_parse_bt_body(b, DS_BT_BODY_MIN, &st) == 0);
	CHECK(check_known(&st));
	memset(&st, 0xEE, sizeof(st));
	CHECK(dualsense_parse_bt_body(b, DS_BT_BODY_MIN - 1, &st) == -1);
	CHECK(st.valid == 0);

	/* the battery level tops out at 10 (nibbles 11 and 15 are clamped, 9 is
	 * not) and the charge nibble picks the flags */
	c[DS_C_STATUS] = 0x2F;
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK(st.battery == 10 && st.charging == 0 && st.full == 1);
	c[DS_C_STATUS] = 0x0B;
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK(st.battery == 10);
	c[DS_C_STATUS] = 0x09;
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK(st.battery == 9);
	c[DS_C_STATUS] = 0x00;
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK(st.battery == 0 && st.charging == 0 && st.full == 0);

	/* hat values: only 0..7 press anything */
	common_known(c);
	c[DS_C_BUTTONS + 0] = 0x00;                 /* hat 0, nothing else */
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK((st.buttons & (DS45_B_UP | DS45_B_RIGHT | DS45_B_DOWN | DS45_B_LEFT)) == DS45_B_UP);
	c[DS_C_BUTTONS + 0] = 0x08;
	place_bt(b, c);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, &st) == 0);
	CHECK((st.buttons & (DS45_B_UP | DS45_B_RIGHT | DS45_B_DOWN | DS45_B_LEFT)) == 0);
	return 0;
}

static int test_parse_links_agree(void)
{
	uint8_t c[DS_C_SIZE], u[64], b[RPT_LEN], bad[RPT_LEN];
	ds45_state_t su, sb, sbody;
	int n, i, bit;

	g_rng = 0x0DDBA11u;
	for (n = 0; n < 200; n++) {
		for (i = 0; i < DS_C_SIZE; i++)
			c[i] = (uint8_t)rnd();
		place_usb(u, c);
		place_bt(b, c);
		for (i = 65; i < 74; i++)               /* the BT-only tail before the CRC */
			b[i] = (uint8_t)rnd();
		seal_crc(b);

		CHECK(dualsense_parse(u, 64, DS45_LINK_USB, &su) == 0);
		CHECK(dualsense_parse(b, RPT_LEN, DS45_LINK_BT, &sb) == 0);
		CHECK(dualsense_parse_bt_body(b, RPT_LEN, &sbody) == 0);
		CHECK(same_state(&su, &sb));
		CHECK(same_state(&sb, &sbody));
	}

	/* the Bluetooth link checks the CRC: no single flipped bit gets through,
	 * wherever it lands, and the CRC-free body parser does not care about the
	 * CRC bytes */
	place_bt(b, c);
	for (i = 0; i < RPT_LEN; i++) {
		for (bit = 0; bit < 8; bit++) {
			memcpy(bad, b, RPT_LEN);
			bad[i] ^= (uint8_t)(1u << bit);
			sb.valid = 1;
			CHECK(dualsense_parse(bad, RPT_LEN, DS45_LINK_BT, &sb) == -1);
			CHECK(sb.valid == 0);
		}
	}
	memcpy(bad, b, RPT_LEN);
	bad[74] ^= 0xFF;
	CHECK(dualsense_parse_bt_body(bad, RPT_LEN, &sbody) == 0);

	/* length and ID are exact for dualsense_parse, per link */
	CHECK(dualsense_parse(b, RPT_LEN - 1, DS45_LINK_BT, &sb) == -1);
	CHECK(dualsense_parse(b, RPT_LEN + 1, DS45_LINK_BT, &sb) == -1);
	CHECK(dualsense_parse(u, 63, DS45_LINK_USB, &su) == -1);
	CHECK(dualsense_parse(u, 65, DS45_LINK_USB, &su) == -1);
	CHECK(dualsense_parse(u, 64, DS45_LINK_BT, &sb) == -1);       /* USB report on BT */
	CHECK(dualsense_parse(b, RPT_LEN, DS45_LINK_USB, &su) == -1); /* BT report on USB */
	CHECK(dualsense_parse(b, 0, DS45_LINK_BT, &sb) == -1);
	CHECK(dualsense_parse(NULL, RPT_LEN, DS45_LINK_BT, &sb) == -1);
	CHECK(dualsense_parse(b, RPT_LEN, DS45_LINK_BT, NULL) == -1);
	CHECK(dualsense_parse_bt_body(NULL, RPT_LEN, &sbody) == -1);
	CHECK(dualsense_parse_bt_body(b, RPT_LEN, NULL) == -1);
	return 0;
}

int run_tests(void)
{
	int (*const tests[])(void) = {
		test_neutral, test_buttons_each, test_buttons_combined,
		test_hat_directions, test_triggers_analog, test_edge_extras_ignored,
		test_map_touchpad_to_ps, test_map_every_target, test_map_over_trigger,
		test_deadzone, test_motion_scale, test_sixaxis_values,
		test_rejects_wrong_id_and_short, test_in_place, test_no_read_past_len,
		test_fuzz, test_basic_neutral, test_basic_buttons, test_basic_triggers,
		test_basic_refuses, test_basic_in_place, test_basic_fuzz_all_lengths,
		test_abi_layout, test_parse_fields, test_parse_links_agree,
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
