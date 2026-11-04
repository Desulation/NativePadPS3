/* SOURCES: ../shim/ds3_output.c */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fakes.h"
#include "../shim/ds3_output.h"

#define SENTINEL   0xA5
#define GUARD      32                  /* guard bytes after the input */

static uint32_t g_rng;

static uint32_t rnd(void)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng >> 8;                  /* the low bits of an LCG are weak */
}

/* ------------------------------------------------------------------------ */
/* Payloads and the independent expectations                                 */
/* ------------------------------------------------------------------------ */

/* The idle DS3 payload the kernel builds: durations 0x96, motors off, no LED. */
static void payload_idle(uint8_t p[DS3_OUT_PAYLOAD_LEN])
{
	memset(p, 0, DS3_OUT_PAYLOAD_LEN);
	p[1] = 0x96;
	p[3] = 0x96;
}

/* Bitwise CRC32 the way the DualSense computes it for an output report: the
 * 0xA2 DATA|OUTPUT header byte first, then report bytes 0..73. Written here
 * on its own; the shim's loop is not shared. */
static uint32_t ref_crc_output(const uint8_t *r)
{
	uint32_t crc = 0xFFFFFFFFu;
	int i, b;

	for (i = -1; i < 74; i++) {
		crc ^= (i < 0) ? 0xA2u : r[i];
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return ~crc;
}

static uint32_t report_crc(const uint8_t *r)
{
	return (uint32_t)r[74] | ((uint32_t)r[75] << 8) |
	       ((uint32_t)r[76] << 16) | ((uint32_t)r[77] << 24);
}

/* Port 1..4 -> player LED byte and colour, the values the task chose. */
static const uint8_t k_led[5]      = { 0x00, 0x04, 0x0A, 0x15, 0x1B };
static const uint8_t k_rgb[5][3]   = {
	{   0,   0,   0 },
	{   0,   0, 255 },                 /* 1 blue  */
	{ 255,   0,   0 },                 /* 2 red   */
	{   0, 255,   0 },                 /* 3 green */
	{ 255,   0, 255 },                 /* 4 pink  */
};

/* Offsets in the report that carry something (everything else is 0). */
static int named_byte(int i)
{
	return i == 0 || i == 1 || i == 2 || i == 3 || i == 4 || i == 5 || i == 6 ||
	       i == 41 || i == 44 || i == 46 || i == 47 || i == 48 || i == 49 ||
	       i >= 74;
}

/* Translate p (48 bytes) with seq into r, requiring success. */
static int translate(const uint8_t *p, uint8_t seq, uint8_t r[DS_OUT_LEN])
{
	memset(r, SENTINEL, DS_OUT_LEN);
	return ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, seq, r);
}

/* Full-report invariants for a payload p translated with seq into r. */
static int report_ok(const uint8_t *p, uint8_t seq, const uint8_t *r)
{
	uint8_t leds = p[9] & 0x1E;
	int port = 0, i;

	for (i = 0; i < 4; i++)
		if (leds & (0x02 << i)) {
			port = i + 1;
			break;
		}

	if (r[0] != 0x31 || r[1] != (uint8_t)((seq & 0x0F) << 4) || r[2] != 0x10)
		return 0;
	if (r[3] != 0x01 || r[4] != 0x14)
		return 0;
	if (r[41] != 0x02 || r[44] != 0x02)
		return 0;
	if (r[5] !=(p[2] ? 255 : 0) || r[6] != p[4])
		return 0;
	if (r[46] != k_led[port])
		return 0;
	if (r[47] != k_rgb[port][0] || r[48] != k_rgb[port][1] || r[49] != k_rgb[port][2])
		return 0;
	for (i = 0; i < 74; i++)
		if (!named_byte(i) && r[i] != 0)
			return 0;
	return report_crc(r) == ref_crc_output(r);
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                     */
/* ------------------------------------------------------------------------ */

static int test_header_and_flags(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN];

	CHECK(DS_OUT_LEN == 78);
	CHECK(DS3_OUT_PAYLOAD_LEN == 48);

	payload_idle(p);
	CHECK(translate(p, 0, r) == 0);
	CHECK(r[0] == 0x31);                 /* report id */
	CHECK(r[1] == 0x00);                 /* sequence nibble, high */
	CHECK(r[2] == 0x10);                 /* tag */
	CHECK(r[3] == 0x01);                 /* valid_flag0: compatible vibration */
	CHECK(r[4] == 0x14);                 /* valid_flag1: light bar colour | player LEDs */
	CHECK(r[11] == 0x00);                /* mute LED */
	CHECK(r[41] == 0x02);                /* valid_flag2: light bar setup enable */
	CHECK(r[44] == 0x02);                /* light bar setup: LIGHT_OUT */
	CHECK(r[45] == 0x00);                /* brightness */

	/* the flags and the setup are set whatever the payload says, every report */
	p[2] = 1;
	p[4] = 0xFF;
	p[9] = 0x1E;
	CHECK(translate(p, 7, r) == 0);
	CHECK(r[3] == 0x01 && r[4] == 0x14);
	CHECK(r[41] == 0x02 && r[44] == 0x02);
	return 0;
}

static int test_rumble_mapping(void)
{
	static const uint8_t k_small[] = { 0, 1, 2, 0x7F, 0x80, 255 };
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN];
	unsigned i;
	int v;

	/* small motor: off stays 0, any non-zero value is 255 */
	for (i = 0; i < sizeof(k_small); i++) {
		payload_idle(p);
		p[2] = k_small[i];
		CHECK(translate(p, 0, r) == 0);
		CHECK(r[5] == (k_small[i] ? 255 : 0));
		CHECK(r[6] == 0);
	}

	/* large motor: copied as is, the small motor is not touched by it */
	for (v = 0; v < 256; v++) {
		payload_idle(p);
		p[4] = (uint8_t)v;
		CHECK(translate(p, 0, r) == 0);
		CHECK(r[6] == (uint8_t)v);
		CHECK(r[5] == 0);
	}

	/* the durations and the request bytes are ignored */
	for (v = 0; v < 256; v++) {
		uint8_t q[DS3_OUT_PAYLOAD_LEN], r2[DS_OUT_LEN];

		payload_idle(p);
		p[2] = 1;
		p[4] = 0x42;
		memcpy(q, p, sizeof(q));
		q[1] = (uint8_t)v;
		q[3] = (uint8_t)~v;
		q[5] = (uint8_t)v;
		q[6] = (uint8_t)(v * 7);
		CHECK(translate(p, 3, r) == 0);
		CHECK(translate(q, 3, r2) == 0);
		CHECK(memcmp(r, r2, DS_OUT_LEN) == 0);
	}

	/* the LED timing blocks are ignored too (steady LEDs) */
	payload_idle(p);
	p[9] = 0x04;
	CHECK(translate(p, 1, r) == 0);
	{
		uint8_t q[DS3_OUT_PAYLOAD_LEN], r2[DS_OUT_LEN];
		int k;

		memcpy(q, p, sizeof(q));
		for (k = 10; k < 30; k++)
			q[k] = (uint8_t)(0xFF - k);
		CHECK(translate(q, 1, r2) == 0);
		CHECK(memcmp(r, r2, DS_OUT_LEN) == 0);
	}
	return 0;
}

static int test_led_and_colour_per_port(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN];
	int port, mask, lowest, i;

	/* each single LED bit: LED 0x02 is port 1 ... 0x10 is port 4 */
	for (port = 1; port <= 4; port++) {
		payload_idle(p);
		p[9] = (uint8_t)(0x01 << port);
		CHECK(translate(p, 0, r) == 0);
		CHECK(r[46] == k_led[port]);
		CHECK(r[47] == k_rgb[port][0]);
		CHECK(r[48] == k_rgb[port][1]);
		CHECK(r[49] == k_rgb[port][2]);
	}

	/* the literal values, so a wrong table on both sides cannot hide */
	payload_idle(p);
	p[9] = 0x02; CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x04 && r[47] == 0 && r[48] == 0 && r[49] == 255);
	p[9] = 0x04; CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x0A && r[47] == 255 && r[48] == 0 && r[49] == 0);
	p[9] = 0x08; CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x15 && r[47] == 0 && r[48] == 255 && r[49] == 0);
	p[9] = 0x10; CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x1B && r[47] == 255 && r[48] == 0 && r[49] == 255);

	/* several bits: the lowest wins */
	for (mask = 1; mask < 16; mask++) {
		lowest = 0;
		for (i = 0; i < 4; i++)
			if (mask & (1 << i)) {
				lowest = i + 1;
				break;
			}
		payload_idle(p);
		p[9] = (uint8_t)(mask << 1);
		CHECK(translate(p, 0, r) == 0);
		CHECK(r[46] == k_led[lowest]);
		CHECK(r[47] == k_rgb[lowest][0] && r[48] == k_rgb[lowest][1] &&
		      r[49] == k_rgb[lowest][2]);
	}

	/* no bit set: LEDs and light bar off */
	payload_idle(p);
	p[9] = 0x00;
	CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0 && r[47] == 0 && r[48] == 0 && r[49] == 0);
	CHECK(r[4] == 0x14);                     /* still told to apply "off" */

	/* bits outside 0x1E are not LEDs */
	payload_idle(p);
	p[9] = 0xE1;
	CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0 && r[47] == 0 && r[48] == 0 && r[49] == 0);
	p[9] = 0xE1 | 0x08;                      /* stray bits plus player 3 */
	CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x15 && r[48] == 255);
	p[9] = 0xFF;                             /* all of them: player 1 */
	CHECK(translate(p, 0, r) == 0);
	CHECK(r[46] == 0x04 && r[49] == 255);
	return 0;
}

static int test_padding_is_zero(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN];
	int i, n;

	/* a busy payload: everything that may carry data does */
	payload_idle(p);
	p[2] = 1;
	p[4] = 0xFF;
	p[5] = 0xFF;
	p[6] = 0xFF;
	p[9] = 0x1E;
	for (i = 10; i < 30; i++)
		p[i] = 0xFF;
	CHECK(translate(p, 15, r) == 0);

	for (i = 0; i < 74; i++)
		if (!named_byte(i))
			CHECK(r[i] == 0);
	/* spot checks on the ones the spec names */
	CHECK(r[7] == 0 && r[10] == 0 && r[11] == 0 && r[40] == 0 && r[42] == 0);
	CHECK(r[43] == 0 && r[45] == 0 && r[50] == 0 && r[73] == 0);
	/* 41 and 44 are the light bar setup, not padding */
	CHECK(r[41] == 0x02 && r[44] == 0x02);

	/* and over a few hundred random payloads */
	g_rng = 0x0DDBA11u;
	for (n = 0; n < 300; n++) {
		for (i = 0; i < DS3_OUT_PAYLOAD_LEN; i++)
			p[i] = (uint8_t)rnd();
		p[7] = p[8] = 0;
		memset(p + 30, 0, 18);
		CHECK(translate(p, (uint8_t)rnd(), r) == 0);
		for (i = 0; i < 74; i++)
			if (!named_byte(i))
				CHECK(r[i] == 0);
		CHECK(r[41] == 0x02 && r[44] == 0x02);
	}
	return 0;
}

static int test_crc_matches_reference(void)
{
	/* The same two reports as a literal, with CRCs computed outside this
	 * program (zlib over 0xA2 + report[0..73]): a bug that is in both bitwise
	 * loops would still fail here. */
	static const uint8_t k_crc_a[4] = { 0x7B, 0x41, 0xFF, 0x41 };
	static const uint8_t k_crc_b[4] = { 0x5A, 0x89, 0xDF, 0x5F };
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN], want[DS_OUT_LEN];
	int n, i;

	/* A: seq 0, small motor on, large 0x80, player 1 */
	payload_idle(p);
	p[2] = 1;
	p[4] = 0x80;
	p[9] = 0x02;
	CHECK(translate(p, 0, r) == 0);
	memset(want, 0, sizeof(want));
	want[0] = 0x31; want[2] = 0x10; want[3] = 0x01; want[4] = 0x14;
	want[41] = 0x02; want[44] = 0x02;
	want[5] = 255;  want[6] = 0x80; want[46] = 0x04; want[49] = 255;
	memcpy(want + 74, k_crc_a, 4);
	CHECK(memcmp(r, want, DS_OUT_LEN) == 0);

	/* B: seq 5, large motor full, player 3 */
	payload_idle(p);
	p[4] = 0xFF;
	p[9] = 0x08;
	CHECK(translate(p, 5, r) == 0);
	memset(want, 0, sizeof(want));
	want[0] = 0x31; want[1] = 0x50; want[2] = 0x10; want[3] = 0x01; want[4] = 0x14;
	want[41] = 0x02; want[44] = 0x02;
	want[6] = 0xFF; want[46] = 0x15; want[48] = 255;
	memcpy(want + 74, k_crc_b, 4);
	CHECK(memcmp(r, want, DS_OUT_LEN) == 0);

	/* and against the reference loop for random payloads */
	g_rng = 0xC2C32u;
	for (n = 0; n < 2000; n++) {
		for (i = 0; i < DS3_OUT_PAYLOAD_LEN; i++)
			p[i] = (uint8_t)rnd();
		p[7] = p[8] = 0;
		memset(p + 30, 0, 18);
		CHECK(translate(p, (uint8_t)rnd(), r) == 0);
		CHECK(report_crc(r) == ref_crc_output(r));
	}
	return 0;
}

static int test_sequence_nibble(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN], r[DS_OUT_LEN], r0[DS_OUT_LEN];
	int s;

	payload_idle(p);
	p[4] = 0x33;
	p[9] = 0x04;
	for (s = 0; s < 256; s++) {
		CHECK(translate(p, (uint8_t)s, r) == 0);
		CHECK(r[1] == (uint8_t)((s & 0x0F) << 4));
		CHECK(r[1] == (uint8_t)((s % 16) * 16));
		CHECK(report_crc(r) == ref_crc_output(r));
	}

	/* 16 wraps to 0: the same report byte for byte, CRC included */
	CHECK(translate(p, 0, r0) == 0);
	CHECK(translate(p, 16, r) == 0);
	CHECK(memcmp(r0, r, DS_OUT_LEN) == 0);
	CHECK(r[1] == 0x00);
	CHECK(translate(p, 15, r) == 0);
	CHECK(r[1] == 0xF0);
	CHECK(translate(p, 0xF5, r) == 0);
	CHECK(r[1] == 0x50);
	return 0;
}

static int test_declines_bad_input(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN + 16], r[DS_OUT_LEN], keep[DS_OUT_LEN];
	int len, i;

	payload_idle(p + 0);
	memset(p + DS3_OUT_PAYLOAD_LEN, 0, 16);
	memset(keep, SENTINEL, sizeof(keep));

	/* NULL in, NULL out */
	memset(r, SENTINEL, sizeof(r));
	CHECK(ds3_output_to_dualsense(NULL, DS3_OUT_PAYLOAD_LEN, 0, r) == -1);
	CHECK(memcmp(r, keep, sizeof(r)) == 0);
	CHECK(ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, 0, NULL) == -1);
	CHECK(ds3_output_to_dualsense(NULL, DS3_OUT_PAYLOAD_LEN, 0, NULL) == -1);

	/* wrong lengths, including those that would hold a payload of the shape */
	for (len = -3; len <= 64; len++) {
		if (len == DS3_OUT_PAYLOAD_LEN)
			continue;
		memset(r, SENTINEL, sizeof(r));
		CHECK(ds3_output_to_dualsense(p, len, 0, r) == -1);
		CHECK(memcmp(r, keep, sizeof(r)) == 0);
	}
	CHECK(ds3_output_to_dualsense(p, 0, 0, r) == -1);
	CHECK(ds3_output_to_dualsense(p, 47, 0, r) == -1);
	CHECK(ds3_output_to_dualsense(p, 49, 0, r) == -1);
	CHECK(ds3_output_to_dualsense(p, 64, 0, r) == -1);

	/* a payload that is not of the shape: in[7], in[8], any of in[30..47] */
	for (i = 0; i < DS3_OUT_PAYLOAD_LEN; i++) {
		if (i != 7 && i != 8 && i < 30)
			continue;
		payload_idle(p);
		p[i] = 1;
		memset(r, SENTINEL, sizeof(r));
		CHECK(ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, 0, r) == -1);
		CHECK(memcmp(r, keep, sizeof(r)) == 0);
		p[i] = 0xFF;
		CHECK(ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, 0, r) == -1);
		CHECK(memcmp(r, keep, sizeof(r)) == 0);
	}

	/* and the shape as given is accepted */
	payload_idle(p);
	CHECK(ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, 0, r) == 0);
	return 0;
}

/* The input sits at the end of area[0..64); GUARD bytes follow it. A different
 * guard must never change the answer, for any length. */
static int test_no_read_past_len(void)
{
	uint8_t area[64 + GUARD], out_a[DS_OUT_LEN], out_b[DS_OUT_LEN];
	int len, n, i, ra, rb;

	g_rng = 0x6A2D0001u;
	for (len = 0; len <= 64; len++) {
		uint8_t *in = area + 64 - len;

		for (n = 0; n < 200; n++) {
			for (i = 0; i < (int)sizeof(area); i++)
				area[i] = (uint8_t)rnd();
			if (len == DS3_OUT_PAYLOAD_LEN && (rnd() & 1)) {
				in[7] = in[8] = 0;               /* often the right shape */
				memset(in + 30, 0, 18);
			}

			memset(out_a, SENTINEL, sizeof(out_a));
			ra = ds3_output_to_dualsense(in, len, 9, out_a);

			for (i = 64; i < (int)sizeof(area); i++)
				area[i] = (uint8_t)~area[i];
			memset(out_b, SENTINEL, sizeof(out_b));
			rb = ds3_output_to_dualsense(in, len, 9, out_b);

			CHECK(ra == rb);
			CHECK(memcmp(out_a, out_b, DS_OUT_LEN) == 0);
			if (len != DS3_OUT_PAYLOAD_LEN)
				CHECK(ra == -1);
		}
	}
	return 0;
}

static int test_fuzz(void)
{
	uint8_t p[96], r[DS_OUT_LEN], keep[DS_OUT_LEN];
	int n, i, len;

	memset(keep, SENTINEL, sizeof(keep));
	g_rng = 0xD5C0FFEEu;
	for (n = 0; n < 20000; n++) {
		uint8_t seq = (uint8_t)rnd();

		for (i = 0; i < 96; i++)
			p[i] = (uint8_t)rnd();

		if (n & 1) {
			/* the right shape: any bytes except in[7], in[8], in[30..47] */
			p[7] = p[8] = 0;
			memset(p + 30, 0, 18);
			if (rnd() & 1)
				p[9] &= 0x1E;               /* as the kernel masks it */
			if (rnd() % 3 == 0)
				p[2] &= 1;                  /* as seen: 0 or 1 */
			memset(r, SENTINEL, sizeof(r));
			CHECK(ds3_output_to_dualsense(p, DS3_OUT_PAYLOAD_LEN, seq, r) == 0);
			CHECK(report_ok(p, seq, r));
		} else {
			/* the wrong shape: a wrong length, or a stray byte */
			if (rnd() & 1) {
				len = (int)(rnd() % 96);
				if (len == DS3_OUT_PAYLOAD_LEN)
					len++;
			} else {
				len = DS3_OUT_PAYLOAD_LEN;
				p[7] = p[8] = 0;
				memset(p + 30, 0, 18);
				i = (int)(rnd() % 20);
				p[i == 0 ? 7 : i == 1 ? 8 : 28 + i] = (uint8_t)(1 + rnd() % 255);
			}
			memset(r, SENTINEL, sizeof(r));
			CHECK(ds3_output_to_dualsense(p, len, seq, r) == -1);
			CHECK(memcmp(r, keep, sizeof(r)) == 0);
		}
	}
	return 0;
}

static int test_probe_report(void)
{
	uint8_t p[DS3_OUT_PAYLOAD_LEN], want[DS_OUT_LEN], got[DS_OUT_LEN];
	int s;

	/* the idle payload with player 1 lit, no rumble */
	payload_idle(p);
	p[9] = 0x02;

	for (s = 0; s < 40; s++) {
		CHECK(translate(p, (uint8_t)s, want) == 0);
		memset(got, SENTINEL, sizeof(got));
		ds3_output_probe((uint8_t)s, got);
		CHECK(memcmp(got, want, DS_OUT_LEN) == 0);
		CHECK(got[5] == 0 && got[6] == 0);          /* no rumble */
		CHECK(got[41] == 0x02 && got[44] == 0x02);  /* light bar setup */
		CHECK(got[46] == 0x04);
		CHECK(got[47] == 0 && got[48] == 0 && got[49] == 255);
		CHECK(report_crc(got) == ref_crc_output(got));
	}
	ds3_output_probe(0xF3, got);
	CHECK(got[1] == 0x30);

	/* a hard-coded report: seq 0, the CRC computed outside this program */
	{
		static const uint8_t k_crc[4] = { 0xA2, 0x12, 0x67, 0xDE };
		uint8_t lit[DS_OUT_LEN];

		memset(lit, 0, sizeof(lit));
		lit[0] = 0x31; lit[2] = 0x10; lit[3] = 0x01; lit[4] = 0x14;
		lit[41] = 0x02; lit[44] = 0x02;
		lit[46] = 0x04; lit[49] = 255;
		memcpy(lit + 74, k_crc, 4);
		ds3_output_probe(0, got);
		CHECK(memcmp(got, lit, DS_OUT_LEN) == 0);
	}

	/* NULL out is a no-op (and does not crash) */
	ds3_output_probe(1, NULL);
	return 0;
}

int run_tests(void)
{
	int (*const tests[])(void) = {
		test_header_and_flags, test_rumble_mapping,
		test_led_and_colour_per_port, test_padding_is_zero,
		test_crc_matches_reference, test_sequence_nibble,
		test_declines_bad_input, test_no_read_past_len, test_fuzz,
		test_probe_report,
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
