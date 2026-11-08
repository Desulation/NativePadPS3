/*
 * NPP kernel shim - the DualShock 3 output payload to a DualSense
 * Bluetooth output report 0x31.
 *
 * Built twice: natively by the host tests, and freestanding into the shim blob
 * that runs inside the PS3 kernel. So no SDK, no libc (hence the byte loops
 * instead of memset), no static data - not even a const table, which would
 * leave a .rodata/TOC entry the blob has to relocate - and nothing that can
 * block. The layouts are described in ds3_output.h.
 */

#include "ds3_output.h"

/* Byte offsets in the DS3 payload. */
#define IN_SMALL_MOTOR      2
#define IN_LARGE_MOTOR      4
#define IN_ZERO_A           7     /* 7 and 8 must be 0 */
#define IN_ZERO_B           8
#define IN_LEDS             9
#define IN_ZERO_TAIL        30    /* 30..47 must be 0 */

/* The bitmap's LED1..LED4 are 0x02, 0x04, 0x08 and 0x10; other bits are not LEDs. */

/* Byte offsets in the DualSense report. */
#define OUT_ID              0
#define OUT_SEQ             1
#define OUT_TAG             2
#define OUT_FLAG0           3
#define OUT_FLAG1           4
#define OUT_MOTOR_RIGHT     5
#define OUT_MOTOR_LEFT      6
#define OUT_FLAG2           41
#define OUT_LIGHTBAR_SETUP  44
#define OUT_PLAYER_LEDS     46
#define OUT_RGB             47    /* R, G, B */
#define OUT_CRC             74    /* the CRC covers bytes 0..73 */

#define REPORT_ID           0x31
#define REPORT_TAG          0x10
/* Rumble needs both bits to work across firmware generations (as SDL sends
 * them): compatible vibration, and haptics select. Firmware 2.21+ Linux uses
 * valid_flag2 bit 2 (compatible vibration 2) instead; not set here. */
#define FLAG0_VIBRATION     0x01  /* compatible vibration */
#define FLAG0_HAPTICS       0x02  /* haptics select */
#define FLAG1_LIGHTBAR      0x04  /* light bar colour enable */
#define FLAG1_PLAYER_LEDS   0x10  /* player indicator enable */
/* Until the host sends the light bar setup, a DualSense on Bluetooth keeps
 * playing its start-up animation on the light bar and ignores the colour. */
#define FLAG2_LIGHTBAR_SETUP 0x02 /* light bar setup control enable */
#define LIGHTBAR_LIGHT_OUT  0x02  /* setup: fade the animation light out */
#define MOTOR_ON            255

#define CRC_PREFIX          0xA2  /* DATA | OUTPUT header byte, not sent */
#define CRC_POLY            0xEDB88320u

/* The power-off feature report: report id, then DS_POWEROFF_PAYLOAD_LEN bytes -
 * the command, zeros, and the CRC at offset 42. */
#define POWEROFF_CMD        0x02
#define POWEROFF_CRC_AT     42    /* the CRC covers payload bytes 0..41 */
#define POWEROFF_CRC_SEED   0x53  /* a feature report's header byte, not sent */

/* The player table: byte n is the value for port n + 1, so a port is a shift
 * of 8 * n bits. (Packed into words, not an array, to keep the code free of
 * static data.) The colours are chosen, not confirmed on hardware: blue, red,
 * green, pink.
 *                                port:   4    3    2    1 */
#define PLAYER_LED_PACK     0x1B150A04u
#define PLAYER_RED_PACK     0xFF00FF00u   /* FF   00   FF   00 */
#define PLAYER_GREEN_PACK   0x00FF0000u   /* 00   FF   00   00 */
#define PLAYER_BLUE_PACK    0xFF0000FFu   /* FF   00   00   FF */

/* The lowest set LED bit of the bitmap as a port 1..4, or 0 when none is. */
static int port_of(uint8_t leds)
{
	/* Spelled out: a loop over the bits can be vectorised into a constant
	 * pool, which is static data. */
	if (leds & 0x02) return 1;
	if (leds & 0x04) return 2;
	if (leds & 0x08) return 3;
	if (leds & 0x10) return 4;
	return 0;
}

static uint8_t pack_byte(uint32_t pack, int port)
{
	return (uint8_t)(pack >> (8 * (port - 1)));
}

/* Reflected CRC-32 of seed followed by p[0..n), bitwise. */
uint32_t ds3_crc32_seeded(uint8_t seed, const uint8_t *p, int n)
{
	uint32_t crc = 0xFFFFFFFFu;
	int i, b;

	for (i = -1; i < n; i++) {
		crc ^= (i < 0) ? (uint32_t)seed : p[i];
		/* With a run-time n clang vectorises the bit loop into shift-count
		 * and polynomial vectors: a constant pool, which is static data. */
#if defined(__clang__)
#pragma clang loop vectorize(disable) interleave(disable) unroll(disable)
#endif
		for (b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (CRC_POLY & (uint32_t)(-(int32_t)(crc & 1)));
	}
	return ~crc;
}

/* Reflected CRC-32 of 0xA2 followed by r[0..OUT_CRC), bitwise. */
static uint32_t crc_output(const uint8_t *r)
{
	return ds3_crc32_seeded(CRC_PREFIX, r, OUT_CRC);
}

/* Build the whole report from the three facts it carries. */
static void build(uint8_t seq, uint8_t small, uint8_t large, uint8_t leds,
                  uint8_t out[DS_OUT_LEN])
{
	int port = port_of(leds);
	uint32_t crc;
	int i;

	for (i = 0; i < DS_OUT_LEN; i++)
		out[i] = 0;

	out[OUT_ID] = REPORT_ID;
	out[OUT_SEQ] = (uint8_t)((seq & 0x0F) << 4);
	out[OUT_TAG] = REPORT_TAG;
	out[OUT_FLAG0] = FLAG0_VIBRATION | FLAG0_HAPTICS;
	out[OUT_FLAG1] = FLAG1_LIGHTBAR | FLAG1_PLAYER_LEDS;
	out[OUT_MOTOR_RIGHT] = small ? MOTOR_ON : 0;
	out[OUT_MOTOR_LEFT] = large;
	out[OUT_FLAG2] = FLAG2_LIGHTBAR_SETUP;
	out[OUT_LIGHTBAR_SETUP] = LIGHTBAR_LIGHT_OUT;

	if (port) {
		out[OUT_PLAYER_LEDS] = pack_byte(PLAYER_LED_PACK, port);
		out[OUT_RGB + 0] = pack_byte(PLAYER_RED_PACK, port);
		out[OUT_RGB + 1] = pack_byte(PLAYER_GREEN_PACK, port);
		out[OUT_RGB + 2] = pack_byte(PLAYER_BLUE_PACK, port);
	}

	crc = crc_output(out);
	out[OUT_CRC + 0] = (uint8_t)crc;
	out[OUT_CRC + 1] = (uint8_t)(crc >> 8);
	out[OUT_CRC + 2] = (uint8_t)(crc >> 16);
	out[OUT_CRC + 3] = (uint8_t)(crc >> 24);
}

int ds3_output_to_dualsense(const uint8_t *in, int in_len, uint8_t seq,
                            uint8_t out[DS_OUT_LEN])
{
	int i;

	if (!in || !out || in_len != DS3_OUT_PAYLOAD_LEN)
		return -1;
	if (in[IN_ZERO_A] != 0 || in[IN_ZERO_B] != 0)
		return -1;
	for (i = IN_ZERO_TAIL; i < DS3_OUT_PAYLOAD_LEN; i++)
		if (in[i] != 0)
			return -1;

	build(seq, in[IN_SMALL_MOTOR], in[IN_LARGE_MOTOR], in[IN_LEDS], out);
	return 0;
}

void ds3_output_probe(uint8_t seq, uint8_t out[DS_OUT_LEN])
{
	if (!out)
		return;
	build(seq, 0, 0, 0x02, out);
}

void ds_poweroff_payload(uint8_t out[DS_POWEROFF_PAYLOAD_LEN])
{
	uint8_t head[POWEROFF_CRC_AT + 1];   /* the CRC prefix after the seed */
	uint32_t crc;
	int i;

	if (!out)
		return;

	/* Zero first, then the command (on x86 a command byte stored ahead of the
	 * zero loop is merged into a constant pool, which is static data). */
	for (i = 0; i < POWEROFF_CRC_AT; i++)
		out[i] = 0;
	out[0] = POWEROFF_CMD;

	/* The CRC covers the seed, the report id, then the 42 bytes before it. */
	for (i = 0; i <= POWEROFF_CRC_AT; i++)
		head[i] = 0;
	head[0] = DS_POWEROFF_REPORT_ID;
	head[1] = POWEROFF_CMD;
	crc = ds3_crc32_seeded(POWEROFF_CRC_SEED, head, POWEROFF_CRC_AT + 1);

	out[POWEROFF_CRC_AT + 0] = (uint8_t)crc;
	out[POWEROFF_CRC_AT + 1] = (uint8_t)(crc >> 8);
	out[POWEROFF_CRC_AT + 2] = (uint8_t)(crc >> 16);
	out[POWEROFF_CRC_AT + 3] = (uint8_t)(crc >> 24);
}
