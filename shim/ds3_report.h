/*
 * NPP kernel shim - DualSense Bluetooth reports 0x31 and 0x01 to a
 * DualShock 3 report.
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
 *   [30]     battery, which the kernel passes on as is: 1..5 the level (the
 *            DualSense's 0..10 halved, rounded up, never 0), 0xEE charging,
 *            0xEF fully charged. The basic report has no battery data and
 *            reads 0x05.
 *   [41..48] sensors, 16 bit big-endian: accel X, Y, Z, then the gyro
 * Every other byte is 0.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef NPP_DS3_REPORT_H
#define NPP_DS3_REPORT_H

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
                              const npp_shim_cfg_t *cfg,
                              uint8_t out[DS3_REPORT_LEN]);

/* The short "basic" Bluetooth report a DualSense sends before it is switched to
 * report 0x31 (DS4-compatible basic layout):
 *   [1..4]  LX LY RX RY
 *   [5]     low nibble hat (0 Up .. 7 Up-Left clockwise, 8 none), Square 0x10,
 *           Cross 0x20, Circle 0x40, Triangle 0x80
 *   [6]     L1 0x01, R1 0x02, L2 0x04, R2 0x08, Create 0x10, Options 0x20,
 *           L3 0x40, R3 0x80
 *   [7]     PS 0x01, touchpad click 0x02 (the other bits are not buttons)
 *   [8..9]  L2, R2 analog
 * It has no motion data: the DS3 sensors read their centre value. */
#define DS_BASIC_REPORT_ID  0x01
#define DS_BASIC_LEN        10     /* id + LX LY RX RY + 3 button bytes + L2 R2 */

/* DualSense Bluetooth basic report in[0..in_len) -> DS3 report. 0 and all
 * DS3_REPORT_LEN bytes of out written, or -1 and out untouched when a pointer is
 * NULL, in[0] != DS_BASIC_REPORT_ID or in_len < DS_BASIC_LEN. Reads nothing past
 * in[in_len-1]; reads all input before writing (in-place safe, out == in). */
int ds3_report_from_dualsense_basic(const uint8_t *in, int in_len,
                                    const npp_shim_cfg_t *cfg,
                                    uint8_t out[DS3_REPORT_LEN]);

#endif /* NPP_DS3_REPORT_H */
