/*
 * DS45PAD - normalised controller state and the DualSense input report layout.
 *
 * What the report parsers and the kernel shim have in common. There are no SDK
 * headers and no libc in here: the shim blob is built freestanding from files
 * that include this one, so it has to stay that way. ds45pad.h includes it for
 * the rest of the plugin.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_STATE_H
#define DS45_STATE_H

#include <stdint.h>

/* Neutral stick value of a DS3 at rest (the sensor one is in sixaxis.h). */
#define DS45_STICK_CENTER       0x0080

/* ------------------------------------------------------------------------ */
/* Normalised controller state                                               */
/*                                                                           */
/* Both HID parsers fill this in; a single translation stage turns it into   */
/* CellPadData. Keeping the parsers free of any libpad knowledge is what     */
/* lets the DS4 and DualSense paths share all downstream behaviour.          */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint16_t x;      /* 0..1919 (DS4) / 0..1919 (DualSense) */
	uint16_t y;      /* 0..941  (DS4) / 0..1079 (DualSense) */
	uint8_t  id;
	uint8_t  active;
} ds45_touch_t;

typedef struct {
	/* Sticks: 0..255, 0x80 centred, Y grows downwards (same as a DS3). */
	uint8_t  lx, ly, rx, ry;

	/* Triggers: 0..255 analog travel. */
	uint8_t  l2, r2;

	/* Digital buttons, DS45_BTN_* independent bitfield. */
	uint32_t buttons;

	/* Motion, raw signed sensor units straight off the controller. */
	int16_t  gyro[3];   /* pitch, yaw, roll */
	int16_t  accel[3];  /* x, y, z          */

	/* Touchpad. */
	ds45_touch_t touch[2];

	/* Power: level 0..10, charging/full flags. */
	uint8_t  battery;
	uint8_t  charging;
	uint8_t  full;

	uint8_t  valid;         /* report parsed successfully              */
	uint8_t  sticks_valid;  /* the stick values came from the device,
	                         * not from an unpopulated report - the
	                         * calibration must never learn from those */
} ds45_state_t;

/* ds45_state_t.buttons bits (parser-neutral, not the libpad layout) */
#define DS45_B_CROSS      0x00000001
#define DS45_B_CIRCLE     0x00000002
#define DS45_B_SQUARE     0x00000004
#define DS45_B_TRIANGLE   0x00000008
#define DS45_B_L1         0x00000010
#define DS45_B_R1         0x00000020
#define DS45_B_L2         0x00000040
#define DS45_B_R2         0x00000080
#define DS45_B_SHARE      0x00000100  /* Share / Create        */
#define DS45_B_OPTIONS    0x00000200
#define DS45_B_L3         0x00000400
#define DS45_B_R3         0x00000800
#define DS45_B_PS         0x00001000
#define DS45_B_TOUCHPAD   0x00002000  /* touchpad click        */
#define DS45_B_MUTE       0x00004000  /* DualSense mic mute    */
#define DS45_B_UP         0x00008000
#define DS45_B_RIGHT      0x00010000
#define DS45_B_DOWN       0x00020000
#define DS45_B_LEFT       0x00040000
#define DS45_B_FN_L       0x00080000  /* DualSense Edge        */
#define DS45_B_FN_R       0x00100000
#define DS45_B_PADDLE_L   0x00200000
#define DS45_B_PADDLE_R   0x00400000

/* Symbolic targets a physical control can be remapped onto. */
typedef enum {
	DS45_MAP_NONE = 0,
	DS45_MAP_CROSS, DS45_MAP_CIRCLE, DS45_MAP_SQUARE, DS45_MAP_TRIANGLE,
	DS45_MAP_L1, DS45_MAP_R1, DS45_MAP_L2, DS45_MAP_R2,
	DS45_MAP_L3, DS45_MAP_R3,
	DS45_MAP_START, DS45_MAP_SELECT, DS45_MAP_PS,
	DS45_MAP_UP, DS45_MAP_RIGHT, DS45_MAP_DOWN, DS45_MAP_LEFT,
	DS45_MAP_MAX
} ds45_map_t;

/* ------------------------------------------------------------------------ */
/* DualSense input report layout                                             */
/* ------------------------------------------------------------------------ */

#define DS_IN_USB           0x01
#define DS_IN_BT            0x31

#define DS_OFF_USB          1
#define DS_OFF_BT           2

/* Offsets inside the 63 byte common payload. */
#define DS_C_LX             0x00
#define DS_C_LY             0x01
#define DS_C_RX             0x02
#define DS_C_RY             0x03
#define DS_C_L2             0x04
#define DS_C_R2             0x05
#define DS_C_SEQ            0x06
#define DS_C_BUTTONS        0x07  /* 4 bytes      */
#define DS_C_GYRO           0x0F  /* 3 x int16 LE */
#define DS_C_ACCEL          0x15  /* 3 x int16 LE */
#define DS_C_TOUCH          0x20  /* 2 x 4 bytes  */
#define DS_C_STATUS         0x34  /* 3 bytes      */
#define DS_C_SIZE           0x3F

#define DS_STATUS0_BATTERY  0x0F
#define DS_STATUS0_CHARGING 0xF0
#define DS_CHARGING_ACTIVE  0x10
#define DS_CHARGING_FULL    0x20

/* The shortest Bluetooth 0x31 report dualsense_parse_bt_body() takes: the
 * report ID and header in front of the common payload, then everything up to
 * and including the status byte, the last field the decoder reads. A whole
 * report is 78 bytes ending in a CRC32; the kernel shim never has the CRC,
 * because the driver's buffer cuts the report at 64 bytes. */
#define DS_BT_BODY_MIN      (DS_OFF_BT + DS_C_STATUS + 1)

/* ------------------------------------------------------------------------ */
/* ds_body.c - the field decoding, shared by dualsense_parse() and the shim  */
/* ------------------------------------------------------------------------ */

/* c points at the common payload (DS_C_SIZE bytes, after the report ID and any
 * Bluetooth header). Fills every field of *st and marks it valid; the caller
 * has already checked the length. */
void dualsense_decode_common(const uint8_t *c, ds45_state_t *st);

/* A Bluetooth 0x31 report with no CRC check. Returns 0 and fills *st, or -1
 * (and clears st->valid) when buf is NULL, buf[0] is not 0x31 or len is below
 * DS_BT_BODY_MIN. Never reads past buf[len - 1]. */
int  dualsense_parse_bt_body(const uint8_t *buf, int len, ds45_state_t *st);

#endif /* DS45_STATE_H */
