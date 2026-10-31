/*
 * DS45PAD kernel shim - DualSense Bluetooth report 0x31 to a DualShock 3 report.
 *
 * The PS3 kernel's DS3 driver only understands the 49 byte DualShock 3 input
 * report (ID 0x01). The shim runs a DualSense's report through this so the
 * driver's own converter takes it from there.
 *
 * Pure logic: no SDK headers, no libc, no state. The same file is built by the
 * host tests and, freestanding, into the shim blob.
 *
 * DS3 report layout, byte offsets, as the kernel converter reads them:
 *   [0]      0x01
 *   [2]      Select 0x01, L3 0x02, R3 0x04, Start 0x08, Up 0x10, Right 0x20,
 *            Down 0x40, Left 0x80
 *   [3]      L2 0x01, R2 0x02, L1 0x04, R1 0x08, Triangle 0x10, Circle 0x20,
 *            Cross 0x40, Square 0x80
 *   [4]      PS 0x01
 *   [6..9]   left X, left Y, right X, right Y
 *   [14..25] pressure: Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle,
 *            Cross, Square
 *   [41..48] sensors, 16 bit big-endian: accel X, Y, Z, then the gyro
 * Every other byte is 0.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_DS3_REPORT_H
#define DS45_DS3_REPORT_H

#include <stdint.h>
#include "shim_abi.h"

#define DS3_REPORT_LEN 49

/* Translate the DualSense Bluetooth report in[0..in_len) into out. The CRC is
 * not checked (the kernel's report buffer is cut before it).
 *
 * Returns 0 and fills all DS3_REPORT_LEN bytes of out, or -1 and leaves out
 * untouched when any pointer is NULL, in[0] is not 0x31 or in_len is shorter
 * than the status byte (DS_BT_BODY_MIN). Reads nothing past in[in_len - 1].
 *
 * Everything is read from in before the first byte of out is written, so out
 * may be the buffer in points into (the shim translates in place); any other
 * overlap is not supported. */
int ds3_report_from_dualsense(const uint8_t *in, int in_len,
                              const ds45_shim_cfg_t *cfg,
                              uint8_t out[DS3_REPORT_LEN]);

#endif /* DS45_DS3_REPORT_H */
