/*
 * DS45PAD - DualSense input report field decoding.
 *
 * The part of the report parsing that needs nothing but the report bytes, so
 * the kernel shim blob can build it freestanding: no SDK headers, no libc, no
 * CRC. The Bluetooth CRC check stays in dualsense.c with the rest of the
 * plugin-side parsing, which calls the decoding here for both links.
 *
 * Note the field order differs from the DualShock 4: the DualSense puts the
 * analog triggers *before* the button bytes, so the two parsers cannot share
 * a common payload layout.
 */

#include "ds45_state.h"

static int16_t rd16le(const uint8_t *p)
{
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* The hat runs clockwise from up in eighths of a turn and reads 8 and above
 * with nothing pressed; a diagonal is the two directions either side of it.
 * Written as comparisons rather than a table so the object has no data
 * section and no TOC entry for the kernel shim blob to relocate. */
static void decode_hat(uint8_t hat, uint32_t *buttons)
{
	if (hat >= 8)
		return;

	if (hat == 7 || hat <= 1)
		*buttons |= DS45_B_UP;
	if (hat >= 1 && hat <= 3)
		*buttons |= DS45_B_RIGHT;
	if (hat >= 3 && hat <= 5)
		*buttons |= DS45_B_DOWN;
	if (hat >= 5 && hat <= 7)
		*buttons |= DS45_B_LEFT;
}

static void decode_touch(const uint8_t *p, ds45_touch_t *t)
{
	t->active = (p[0] & 0x80) ? 0 : 1;
	t->id     = p[0] & 0x7F;
	t->x      = (uint16_t)p[1] | (((uint16_t)p[2] & 0x0F) << 8);
	t->y      = (uint16_t)((p[2] & 0xF0) >> 4) | ((uint16_t)p[3] << 4);
}

uint32_t dualsense_decode_buttons(uint8_t b0, uint8_t b1, uint8_t b2)
{
	uint32_t buttons = 0;

	decode_hat(b0 & 0x0F, &buttons);
	if (b0 & 0x10) buttons |= DS45_B_SQUARE;
	if (b0 & 0x20) buttons |= DS45_B_CROSS;
	if (b0 & 0x40) buttons |= DS45_B_CIRCLE;
	if (b0 & 0x80) buttons |= DS45_B_TRIANGLE;

	if (b1 & 0x01) buttons |= DS45_B_L1;
	if (b1 & 0x02) buttons |= DS45_B_R1;
	if (b1 & 0x04) buttons |= DS45_B_L2;
	if (b1 & 0x08) buttons |= DS45_B_R2;
	if (b1 & 0x10) buttons |= DS45_B_SHARE;    /* Create */
	if (b1 & 0x20) buttons |= DS45_B_OPTIONS;
	if (b1 & 0x40) buttons |= DS45_B_L3;
	if (b1 & 0x80) buttons |= DS45_B_R3;

	if (b2 & 0x01) buttons |= DS45_B_PS;
	if (b2 & 0x02) buttons |= DS45_B_TOUCHPAD;
	if (b2 & 0x04) buttons |= DS45_B_MUTE;

	/* DualSense Edge extras. Mainline Linux does not decode these; the bit
	 * positions below are the community documented layout and are worth
	 * re-checking with the diagnostic dump on an actual Edge. */
	if (b2 & 0x10) buttons |= DS45_B_FN_L;
	if (b2 & 0x20) buttons |= DS45_B_FN_R;
	if (b2 & 0x40) buttons |= DS45_B_PADDLE_L;
	if (b2 & 0x80) buttons |= DS45_B_PADDLE_R;

	return buttons;
}

void dualsense_decode_common(const uint8_t *c, ds45_state_t *st)
{
	int i;
	uint8_t status0, charge;

	st->lx = c[DS_C_LX];
	st->ly = c[DS_C_LY];
	st->rx = c[DS_C_RX];
	st->ry = c[DS_C_RY];
	st->l2 = c[DS_C_L2];
	st->r2 = c[DS_C_R2];

	/* the fourth button byte is a counter: ignored */
	st->buttons = dualsense_decode_buttons(c[DS_C_BUTTONS + 0],
	                                       c[DS_C_BUTTONS + 1],
	                                       c[DS_C_BUTTONS + 2]);

	for (i = 0; i < 3; i++) {
		st->gyro[i]  = rd16le(c + DS_C_GYRO  + i * 2);
		st->accel[i] = rd16le(c + DS_C_ACCEL + i * 2);
	}

	decode_touch(c + DS_C_TOUCH + 0, &st->touch[0]);
	decode_touch(c + DS_C_TOUCH + 4, &st->touch[1]);

	status0      = c[DS_C_STATUS];
	st->battery  = status0 & DS_STATUS0_BATTERY;
	charge       = status0 & DS_STATUS0_CHARGING;
	st->charging = (charge == DS_CHARGING_ACTIVE);
	st->full     = (charge == DS_CHARGING_FULL);
	if (st->battery > 10)
		st->battery = 10;

	st->valid = 1;
	st->sticks_valid = 1;
}

int dualsense_parse_bt_body(const uint8_t *buf, int len, ds45_state_t *st)
{
	if (!st)
		return -1;
	st->valid = 0;
	if (!buf || len < DS_BT_BODY_MIN || buf[0] != DS_IN_BT)
		return -1;

	dualsense_decode_common(buf + DS_OFF_BT, st);
	return 0;
}
