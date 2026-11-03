/*
 * DS45PAD - DualSense / DualSense Edge HID report translation.
 *
 *   USB input  : report 0x01, 64 bytes, payload starts at offset 1
 *   BT  input  : report 0x31, 78 bytes, payload starts at offset 2,
 *                trailing 4 bytes are a CRC32
 *   USB output : report 0x02, 63 bytes
 *   BT  output : report 0x31, 78 bytes, payload at offset 3, trailing CRC32
 *
 * Note the field order differs from the DualShock 4: the DualSense puts the
 * analog triggers *before* the button bytes, so the two parsers cannot share
 * a common payload layout.
 *
 * The input half is split in two. The report layout (DS_IN_*, DS_OFF_*,
 * DS_C_*) is in ds45_state.h and the field decoding in ds_body.c, because the
 * kernel shim builds those freestanding; what stays here is what needs the
 * plugin: the framing checks, the Bluetooth CRC and the output reports.
 */

#include "ds45pad.h"

#define DS_OUT_USB          0x02
#define DS_OUT_BT           0x31

#define DS_OUT_USB_SIZE     63
#define DS_OUT_BT_SIZE      78
#define DS_OUT_TAG          0x10

#define DS_CRC_SEED_OUT     0xA2

/* Output report field offsets, relative to the start of the common block. */
#define DS_O_VALID0         0x00
#define DS_O_VALID1         0x01
#define DS_O_MOTOR_RIGHT    0x02
#define DS_O_MOTOR_LEFT     0x03
#define DS_O_VALID2         0x26
#define DS_O_LIGHTBAR_SETUP 0x29
#define DS_O_LED_BRIGHTNESS 0x2A
#define DS_O_PLAYER_LEDS    0x2B
#define DS_O_LIGHTBAR_R     0x2C
#define DS_O_LIGHTBAR_G     0x2D
#define DS_O_LIGHTBAR_B     0x2E

#define DS_VALID0_COMPAT_VIBRATION   0x01
#define DS_VALID0_HAPTICS_SELECT     0x02
#define DS_VALID1_LIGHTBAR_CONTROL   0x04
#define DS_VALID1_PLAYER_INDICATOR   0x10
#define DS_VALID2_LIGHTBAR_SETUP     0x02
#define DS_VALID2_COMPAT_VIBRATION2  0x04
#define DS_LIGHTBAR_SETUP_LIGHT_OUT  0x02

int dualsense_parse(const uint8_t *buf, int len, ds45_link_t link,
                    ds45_state_t *st)
{
	if (!st)
		return -1;
	st->valid = 0;
	if (!buf || len <= 0)
		return -1;

	if (link == DS45_LINK_BT) {
		/* The DualSense only emits the full 0x31 report once the host has
		 * read its calibration feature report. The abbreviated 0x01 report
		 * it sends before that is not publicly documented, so we ignore it
		 * rather than guess at a layout. */
		if (buf[0] != DS_IN_BT || len != 78)
			return -1;
		{
			uint8_t seed = 0xA1; /* INPUT reports; output uses 0xA2. */
			uint32_t crc = ds45_crc32(0xFFFFFFFFu, &seed, 1);
			uint32_t expected = (uint32_t)buf[74] | ((uint32_t)buf[75] << 8) |
			                    ((uint32_t)buf[76] << 16) | ((uint32_t)buf[77] << 24);
			if (~ds45_crc32(crc, buf, 74) != expected)
				return -1;
		}
		/* The CRC holds; the fields come from the decoding the kernel shim
		 * shares (ds_body.c). */
		return dualsense_parse_bt_body(buf, len, st);
	}

	if (link != DS45_LINK_USB || buf[0] != DS_IN_USB || len != 64)
		return -1;
	dualsense_decode_common(buf + DS_OFF_USB, st);
	return 0;
}

int dualsense_build_output(uint8_t *buf, int cap, ds45_link_t link,
                           const ds45_output_t *out)
{
	/* Player indicator patterns for the five LEDs under the touchpad. */
	static const uint8_t player_led[4] = { 0x04, 0x0A, 0x15, 0x1B };
	int size, o;

	size = (link == DS45_LINK_BT) ? DS_OUT_BT_SIZE : DS_OUT_USB_SIZE;
	if (cap < size)
		return -1;

	for (o = 0; o < size; o++)
		buf[o] = 0;

	if (link == DS45_LINK_BT) {
		buf[0] = DS_OUT_BT;
		buf[1] = 0x00;          /* sequence tag, incremented by the caller */
		buf[2] = DS_OUT_TAG;
		o = 3;
	} else {
		buf[0] = DS_OUT_USB;
		o = 1;
	}

	/* COMPATIBLE_VIBRATION drives the classic rumble motors. Newer DualSense
	 * firmware also wants VIBRATION2 in valid_flag2 or the motors stay
	 * silent, so both are set. */
	buf[o + DS_O_VALID0] = DS_VALID0_COMPAT_VIBRATION | DS_VALID0_HAPTICS_SELECT;
	buf[o + DS_O_VALID1] = DS_VALID1_LIGHTBAR_CONTROL | DS_VALID1_PLAYER_INDICATOR;
	buf[o + DS_O_VALID2] = DS_VALID2_COMPAT_VIBRATION2 | DS_VALID2_LIGHTBAR_SETUP;

	buf[o + DS_O_MOTOR_RIGHT]    = out->rumble_weak;
	buf[o + DS_O_MOTOR_LEFT]     = out->rumble_strong;

	/* Skip the power-on lightbar fade so the colour we pick shows up at once. */
	buf[o + DS_O_LIGHTBAR_SETUP] = DS_LIGHTBAR_SETUP_LIGHT_OUT;
	buf[o + DS_O_LED_BRIGHTNESS] = 0x00;
	buf[o + DS_O_PLAYER_LEDS]    = (out->player >= 1 && out->player <= 4)
	                                ? player_led[out->player - 1] : 0x00;
	buf[o + DS_O_LIGHTBAR_R]     = out->led_r;
	buf[o + DS_O_LIGHTBAR_G]     = out->led_g;
	buf[o + DS_O_LIGHTBAR_B]     = out->led_b;

	if (link == DS45_LINK_BT) {
		uint8_t seed = DS_CRC_SEED_OUT;
		uint32_t crc = ds45_crc32(0xFFFFFFFF, &seed, 1);
		crc = ~ds45_crc32(crc, buf, size - 4);
		buf[size - 4] = (uint8_t)(crc & 0xFF);
		buf[size - 3] = (uint8_t)((crc >> 8) & 0xFF);
		buf[size - 2] = (uint8_t)((crc >> 16) & 0xFF);
		buf[size - 1] = (uint8_t)((crc >> 24) & 0xFF);
	}

	return size;
}
