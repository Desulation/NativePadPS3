/*
 * NPP kernel shim - DualSense Bluetooth reports 0x31 and 0x01 to a
 * DualShock 3 report.
 *
 * Built twice: natively by the host tests, and freestanding into the shim blob
 * that runs inside the PS3 kernel's DS3 driver. So no SDK, no libc (hence the
 * byte loops instead of memset), no static state, nothing that can block.
 * The report layout is described in ds3_report.h.
 */

#include "ds3_report.h"
#include "../src/npp_state.h"
#include "../src/sixaxis.h"

/* Byte offsets in the DS3 report. */
#define DS3_ID          0
#define DS3_DIGITAL1    2
#define DS3_DIGITAL2    3
#define DS3_PS          4
#define DS3_STICKS      6     /* LX, LY, RX, RY */
#define DS3_PRESS       14    /* 12 pressure bytes, see the PR_* names */
#define DS3_BATTERY     30    /* the battery byte the kernel passes on as is */
#define DS3_SENSORS     41    /* accel X, Y, Z, gyro: 16 bit big-endian */

/* DS3_BATTERY values */
#define BATT_FULL_LEVEL 0x05  /* top of the 1..5 level scale */
#define BATT_CHARGING   0xEE
#define BATT_CHARGED    0xEF

/* DS3_DIGITAL1 */
#define D1_SELECT       0x01
#define D1_L3           0x02
#define D1_R3           0x04
#define D1_START        0x08
#define D1_UP           0x10
#define D1_RIGHT        0x20
#define D1_DOWN         0x40
#define D1_LEFT         0x80

/* DS3_DIGITAL2 */
#define D2_L2           0x01
#define D2_R2           0x02
#define D2_L1           0x04
#define D2_R1           0x08
#define D2_TRIANGLE     0x10
#define D2_CIRCLE       0x20
#define D2_CROSS        0x40
#define D2_SQUARE       0x80

/* DS3_PS */
#define PS_BUTTON       0x01

/* Pressure bytes. */
#define PR_UP           (DS3_PRESS + 0)
#define PR_RIGHT        (DS3_PRESS + 1)
#define PR_DOWN         (DS3_PRESS + 2)
#define PR_LEFT         (DS3_PRESS + 3)
#define PR_L2           (DS3_PRESS + 4)
#define PR_R2           (DS3_PRESS + 5)
#define PR_L1           (DS3_PRESS + 6)
#define PR_R1           (DS3_PRESS + 7)
#define PR_TRIANGLE     (DS3_PRESS + 8)
#define PR_CIRCLE       (DS3_PRESS + 9)
#define PR_CROSS        (DS3_PRESS + 10)
#define PR_SQUARE       (DS3_PRESS + 11)

#define PRESSED         255

/* Press one DS3 control: its digital bit and, for the twelve that have one, a
 * full-scale pressure byte. A target that is not a npp_map_t value does
 * nothing. */
static void press(uint8_t *out, int target)
{
	switch (target) {
	case NPP_MAP_CROSS:
		out[DS3_DIGITAL2] |= D2_CROSS;    out[PR_CROSS] = PRESSED;    break;
	case NPP_MAP_CIRCLE:
		out[DS3_DIGITAL2] |= D2_CIRCLE;   out[PR_CIRCLE] = PRESSED;   break;
	case NPP_MAP_SQUARE:
		out[DS3_DIGITAL2] |= D2_SQUARE;   out[PR_SQUARE] = PRESSED;   break;
	case NPP_MAP_TRIANGLE:
		out[DS3_DIGITAL2] |= D2_TRIANGLE; out[PR_TRIANGLE] = PRESSED; break;
	case NPP_MAP_L1:
		out[DS3_DIGITAL2] |= D2_L1;       out[PR_L1] = PRESSED;       break;
	case NPP_MAP_R1:
		out[DS3_DIGITAL2] |= D2_R1;       out[PR_R1] = PRESSED;       break;
	case NPP_MAP_L2:
		out[DS3_DIGITAL2] |= D2_L2;       out[PR_L2] = PRESSED;       break;
	case NPP_MAP_R2:
		out[DS3_DIGITAL2] |= D2_R2;       out[PR_R2] = PRESSED;       break;
	case NPP_MAP_L3:
		out[DS3_DIGITAL1] |= D1_L3;                                   break;
	case NPP_MAP_R3:
		out[DS3_DIGITAL1] |= D1_R3;                                   break;
	case NPP_MAP_START:
		out[DS3_DIGITAL1] |= D1_START;                                break;
	case NPP_MAP_SELECT:
		out[DS3_DIGITAL1] |= D1_SELECT;                               break;
	case NPP_MAP_PS:
		out[DS3_PS] |= PS_BUTTON;                                     break;
	case NPP_MAP_UP:
		out[DS3_DIGITAL1] |= D1_UP;       out[PR_UP] = PRESSED;       break;
	case NPP_MAP_RIGHT:
		out[DS3_DIGITAL1] |= D1_RIGHT;    out[PR_RIGHT] = PRESSED;    break;
	case NPP_MAP_DOWN:
		out[DS3_DIGITAL1] |= D1_DOWN;     out[PR_DOWN] = PRESSED;     break;
	case NPP_MAP_LEFT:
		out[DS3_DIGITAL1] |= D1_LEFT;     out[PR_LEFT] = PRESSED;     break;
	default:
		break;
	}
}

/* A stick inside the deadzone reads as centred. `deadzone` is a percentage of
 * the half travel (128 steps) and the edge itself counts as inside. */
static uint8_t stick(uint8_t v, uint8_t deadzone)
{
	int d = (int)v - NPP_STICK_CENTER;

	if (d < 0)
		d = -d;

	return (d * 100 <= (int)deadzone * 128) ? (uint8_t)NPP_STICK_CENTER : v;
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xFF);
}

/* Write the whole DS3 report but the sensor words (left 0) from the decoded
 * buttons, triggers and sticks in *st: the part both DualSense reports share.
 * Reads only lx, ly, rx, ry, l2, r2 and buttons. */
static void write_ds3(const npp_state_t *st, const npp_shim_cfg_t *cfg,
                      uint8_t *out)
{
	uint32_t b;
	int i;

	for (i = 0; i < DS3_REPORT_LEN; i++)
		out[i] = 0;
	out[DS3_ID] = 0x01;

	/* --- digital buttons ----------------------------------------------- */
	b = st->buttons;
	if (b & NPP_B_SHARE)    press(out, NPP_MAP_SELECT);   /* Create  */
	if (b & NPP_B_OPTIONS)  press(out, NPP_MAP_START);
	if (b & NPP_B_L3)       press(out, NPP_MAP_L3);
	if (b & NPP_B_R3)       press(out, NPP_MAP_R3);
	if (b & NPP_B_PS)       press(out, NPP_MAP_PS);
	if (b & NPP_B_UP)       press(out, NPP_MAP_UP);
	if (b & NPP_B_RIGHT)    press(out, NPP_MAP_RIGHT);
	if (b & NPP_B_DOWN)     press(out, NPP_MAP_DOWN);
	if (b & NPP_B_LEFT)     press(out, NPP_MAP_LEFT);
	if (b & NPP_B_SQUARE)   press(out, NPP_MAP_SQUARE);
	if (b & NPP_B_CROSS)    press(out, NPP_MAP_CROSS);
	if (b & NPP_B_CIRCLE)   press(out, NPP_MAP_CIRCLE);
	if (b & NPP_B_TRIANGLE) press(out, NPP_MAP_TRIANGLE);
	if (b & NPP_B_L1)       press(out, NPP_MAP_L1);
	if (b & NPP_B_R1)       press(out, NPP_MAP_R1);

	/* The triggers are analog on both controllers: the pressure bytes are the
	 * travel itself, and only the digital bit follows the DualSense's flag. */
	if (b & NPP_B_L2)       out[DS3_DIGITAL2] |= D2_L2;
	if (b & NPP_B_R2)       out[DS3_DIGITAL2] |= D2_R2;
	out[PR_L2] = st->l2;
	out[PR_R2] = st->r2;

	/* The remappable extras go last, so a mapped L2 or R2 reads full pressure
	 * whatever the real trigger is doing. */
	if (b & NPP_B_TOUCHPAD) press(out, cfg->map_touchpad);
	if (b & NPP_B_MUTE)     press(out, cfg->map_mute);

	/* --- sticks -------------------------------------------------------- */
	out[DS3_STICKS + 0] = stick(st->lx, cfg->deadzone);
	out[DS3_STICKS + 1] = stick(st->ly, cfg->deadzone);
	out[DS3_STICKS + 2] = stick(st->rx, cfg->deadzone);
	out[DS3_STICKS + 3] = stick(st->ry, cfg->deadzone);
}

/* The DS3 battery byte for a decoded report: 0xEE while charging, 0xEF when
 * full, otherwise the level 1..5 (the DualSense's 0..10 halved, rounded up; 0
 * is never sent because on the DS3 scale it means empty, and the DualSense's 0
 * only means under 10%). */
static uint8_t battery_byte(const npp_state_t *st)
{
	int level;

	if (st->charging)
		return BATT_CHARGING;
	if (st->full)
		return BATT_CHARGED;

	level = ((int)st->battery + 1) / 2;
	if (level < 1)
		level = 1;
	if (level > BATT_FULL_LEVEL)
		level = BATT_FULL_LEVEL;
	return (uint8_t)level;
}

/* All four sensor words at the DS3's rest value. */
static void sensors_centre(uint8_t *out)
{
	int i;

	for (i = 0; i < 4; i++)
		put_be16(out + DS3_SENSORS + 2 * i, NPP_SENSOR_CENTER);
}

int ds3_report_from_dualsense(const uint8_t *in, int in_len,
                              const npp_shim_cfg_t *cfg,
                              uint8_t out[DS3_REPORT_LEN])
{
	npp_state_t st;
	int ax, ay;

	if (!in || !cfg || !out)
		return -1;
	if (dualsense_parse_bt_body(in, in_len, &st) != 0)
		return -1;

	/* st has all of the report now, so from here on out may be the buffer
	 * in points into. */
	write_ds3(&st, cfg, out);
	out[DS3_BATTERY] = battery_byte(&st);

	/* --- motion -------------------------------------------------------- */
	if (cfg->sixaxis) {
		ax = cfg->swap_xy ? 1 : 0;
		ay = cfg->swap_xy ? 0 : 1;

		put_be16(out + DS3_SENSORS + 0,
		         sixaxis_from_accel(st.accel[ax], cfg->accel_scale, cfg->invert_x));
		put_be16(out + DS3_SENSORS + 2,
		         sixaxis_from_accel(st.accel[ay], cfg->accel_scale, cfg->invert_y));
		put_be16(out + DS3_SENSORS + 4,
		         sixaxis_from_accel(st.accel[2], cfg->accel_scale, cfg->invert_z));
		put_be16(out + DS3_SENSORS + 6,
		         sixaxis_from_gyro(st.gyro[1], cfg->gyro_scale, cfg->invert_g));
	} else {
		sensors_centre(out);
	}

	return 0;
}

/* Byte offsets in the basic report, and the bits of its third button byte
 * that are buttons (PS and the touchpad click; the rest are not). */
#define BASIC_STICKS    1     /* LX, LY, RX, RY */
#define BASIC_BUTTONS   5     /* 3 bytes, the DualSense button byte layout */
#define BASIC_L2        8
#define BASIC_R2        9
#define BASIC_B2_MASK   0x03

int ds3_report_from_dualsense_basic(const uint8_t *in, int in_len,
                                    const npp_shim_cfg_t *cfg,
                                    uint8_t out[DS3_REPORT_LEN])
{
	npp_state_t st;

	if (!in || !cfg || !out)
		return -1;
	if (in_len < DS_BASIC_LEN || in[0] != DS_BASIC_REPORT_ID)
		return -1;

	/* Only the fields write_ds3() reads. */
	st.lx = in[BASIC_STICKS + 0];
	st.ly = in[BASIC_STICKS + 1];
	st.rx = in[BASIC_STICKS + 2];
	st.ry = in[BASIC_STICKS + 3];
	st.l2 = in[BASIC_L2];
	st.r2 = in[BASIC_R2];
	st.buttons = dualsense_decode_buttons(in[BASIC_BUTTONS + 0],
	                                      in[BASIC_BUTTONS + 1],
	                                      in[BASIC_BUTTONS + 2] & BASIC_B2_MASK);

	/* everything is read: out may be the buffer in points into */
	write_ds3(&st, cfg, out);
	sensors_centre(out);
	out[DS3_BATTERY] = BATT_FULL_LEVEL;   /* the basic report has no battery data */
	return 0;
}
