/*
 * NPP - DualShock 4 / DualSense motion to DualShock 3 sixaxis scaling.
 *
 * Split out of padldd.c so the kernel shim blob can build it freestanding: it
 * needs nothing but integer arithmetic.
 */

#include "sixaxis.h"

static int clamp(int v, int lo, int hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

uint16_t sixaxis_from_accel(int16_t raw, int scale, int invert)
{
	/* DS4/DualSense report 8192 LSB per g. A DualShock 3 sits at 512 with
	 * roughly 113 LSB per g, so the physically correct conversion is
	 * 512 + raw * 113 / 8192. Divide before applying the user scale to keep
	 * the intermediate inside 32 bits. */
	int v = ((int)raw * 113) / 8192;

	v = (v * scale) / 100;
	if (invert)
		v = -v;

	return (uint16_t)clamp(v + NPP_SENSOR_CENTER, 0, 1023);
}

uint16_t sixaxis_from_gyro(int16_t raw, int scale, int invert)
{
	/* The gyro scale has no clean physical equivalent on a DS3 - the raw
	 * DS4/DualSense counts are divided down to land inside the 10 bit range
	 * and GYRO_SCALE tunes it from there. */
	int v = ((int)raw) / 32;

	v = (v * scale) / 100;
	if (invert)
		v = -v;

	return (uint16_t)clamp(v + NPP_SENSOR_CENTER, 0, 1023);
}
