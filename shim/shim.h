/*
 * DS45PAD kernel shim - host-testable decision logic.
 *
 * The three wrappers the plugin patches the kernel's DS3 driver with (start-up
 * F1, input F2, product-ID F4) split into two layers:
 *
 *   - this file / shim.c: the decisions, taking the data block and the
 *     already-extracted arguments (VID/PID, buffer, length) explicitly, so they
 *     are pure and host-testable with no kernel calls;
 *   - shim/entry.c: the thin, untested kernel-ABI glue that reads those
 *     arguments from the real F1/F2 argument lists and the data block, calls
 *     the function here, and tail-calls the original for anything that is not
 *     an enabled DualSense.
 *
 * It compiles INTO the freestanding kernel blob as well as the host tests, so
 * like ds3_report.c it includes only <stdint.h>, shim_abi.h, ds3_report.h and
 * shim.h, uses no libc and declares no static data.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_SHIM_H
#define DS45_SHIM_H

#include <stdint.h>
#include "shim_abi.h"

/* F3 GET_REPORT of the DualSense calibration feature report. Reading HID
 * feature report 0x05 over Bluetooth is what switches a DualSense from its
 * minimal input report to the full motion report 0x31. The plugin supplies the
 * get_report primitive (it builds the 0x43/0x05 control-channel PDU and drives
 * it through the kernel BT-HID sender, fact sheet F3); the shim only has to ask
 * for the right report type and id. The type is the GET_REPORT header's low
 * nibble: 0x40 (GET_REPORT) | 0x03 (feature). */
#define SHIM_HID_FEATURE          0x03
#define SHIM_DS_CALIB_REPORT_ID   0x05

/* F1 start-up return values. The original Bluetooth start-up handler returns 0
 * on its success path and a negative LV2 error on failure (fact sheet F1). The
 * shim returns SHIM_F1_OK so the driver keeps and polls the pad; the real
 * 3->running transition then comes from the input wrapper translating each 0x31
 * report into a DS3 0x01 report (which satisfies the handler's gate), not from
 * this return. SHIM_F1_ERR is the negative-error path (F1's example error). */
#define SHIM_F1_OK    0
#define SHIM_F1_ERR   ((int)0x8001000CU)

/* The plugin-supplied BT-HID GET_REPORT primitive held in
 * ds45_shim_data_t.get_report (as a function descriptor). It issues a
 * GET_REPORT of (report_type, report_id) on the connection ctx identifies and
 * returns 0 on success or a negative LV2 error. */
typedef int (*shim_get_report_fn)(void *ctx, uint8_t report_type,
                                  uint8_t report_id);

/* 1 only for a DualSense (VID 0x054C, PID 0x0CE6). */
int shim_is_dualsense(uint16_t vid, uint16_t pid);

/* F4 product-ID helper. 1 for VID 0x054C with PID 0x0268, 0x042F or 0x0CE6,
 * else 0. It is the one function the kernel calls by its raw code address with
 * the kernel's own r2, so it must stay a leaf that touches no global and no TOC
 * (tools/shim_pack.py enforces that). */
int shim_pid_ok(uint32_t vid, uint32_t pid);

/* F2 input filter, run after the original read has filled buf. For an enabled
 * DualSense whose buffer holds a 0x31 report that translates, buf is rewritten
 * in place to a 49-byte DS3 report, *len becomes 49 and stat_translated is
 * bumped; otherwise buf and *len are left untouched and stat_passed is bumped.
 * *len carries the original byte count in and the resulting count out (the F2
 * length is passed as a half-word, so uint16_t), and the return value is the
 * resulting count (the F2 return value). d->enabled == 0 forces passthrough. */
int shim_input_filter(ds45_shim_data_t *d, uint16_t vid, uint16_t pid,
                      uint8_t *buf, uint16_t *len);

/* F1 start-up for a DualSense: issue the feature-0x05 GET through
 * d->get_report so the pad begins emitting 0x31 reports, bump stat_startups on
 * success, and return F1's running condition (SHIM_F1_OK) or the GET's error.
 * drv is the F1 start-up argument (the connection the GET targets); arg1 is
 * taken for signature fidelity with F1 - entry.c has already routed the
 * original's arg1==0 early-return to d->orig_startup, so the running transition
 * never depends on it here. */
int shim_startup_dualsense(ds45_shim_data_t *d, void *drv, long arg1);

#endif /* DS45_SHIM_H */
