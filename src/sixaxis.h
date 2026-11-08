/*
 * NPP - DualShock 4 / DualSense motion to DualShock 3 sixaxis scaling.
 *
 * No SDK headers and no libc: the kernel shim blob builds sixaxis.c as well as
 * the plugin does.
 */
#ifndef NPP_SIXAXIS_H
#define NPP_SIXAXIS_H

#include <stdint.h>

/* The neutral sensor value of a DS3 at rest (the stick one is in npp_state.h). */
#define NPP_SENSOR_CENTER      0x0200  /* 512, sensors are 10-bit */

/* Raw sensor counts to a DS3 sensor word, 0..1023 around NPP_SENSOR_CENTER.
 * `scale` is a percentage of the physical value (100 = as measured) and
 * `invert` flips the sign before the centre is added. */
uint16_t sixaxis_from_accel(int16_t raw, int scale, int invert);
uint16_t sixaxis_from_gyro(int16_t raw, int scale, int invert);

#endif /* NPP_SIXAXIS_H */
