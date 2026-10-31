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
 * minimal input report to the full motion report 0x31. The kernel has no GET
 * helper (the DS3 driver only ever SETs), so the shim builds the control-channel
 * request PDU itself - one header byte, then the report id, no payload - and
 * hands it to the kernel's own Bluetooth HID transfer routine (note
 * 2026-10-31-ds3-route-get-primitive.md, P1). The header is the HIDP
 * transaction GET_REPORT (0x40) | the report type (feature = 0x03) = 0x43. */
#define SHIM_HIDP_GET_REPORT      0x40
#define SHIM_HID_FEATURE          0x03
#define SHIM_DS_CALIB_REPORT_ID   0x05
#define SHIM_GET_PDU_LEN          2

/* How the transfer routine is called for that PDU (P1, cross-checked against
 * the driver's own SET sender, which also uses dir 2 / opclass 2): dir selects
 * the L2CAP channel slot and its direction - 2 is the control channel's TX - and
 * opclass 2 is the HID report transfer. */
#define SHIM_SEND_DIR_CTRL_OUT    2
#define SHIM_SEND_OPCLASS_REPORT  2

/* How many times the start-up sends the GET for one connection (slot). The
 * driver may call the start-up again and again for a connection - the original
 * steps a persisted substate machine and the shim never advances it (fact sheet
 * F1) - and every GET that goes out leaves a reply in a queue nothing drains if
 * the pad never switches. So the sends are bounded rather than limited to one:
 * a lost or refused GET can still be retried, a few times. The count is kept per
 * slot in ds45_shim_data_t.start_tries and re-armed by the plugin. */
#define SHIM_START_TRIES_MAX      8

/* F1 start-up return values. The original Bluetooth start-up handler returns 0
 * on its success path and a negative LV2 error on failure (fact sheet F1). The
 * shim returns SHIM_F1_OK so the driver keeps and polls the pad; the real
 * 3->running transition then comes from the input wrapper translating each 0x31
 * report into a DS3 0x01 report (which satisfies the handler's gate), not from
 * this return. SHIM_F1_ERR is the negative-error path (F1's example error). */
#define SHIM_F1_OK    0
#define SHIM_F1_ERR   ((int)0x8001000CU)

/* The kernel's Bluetooth HID transfer routine (note P1), called through the
 * function descriptor ds45_shim_data_t.send_routine points at:
 *     int h116ef0(u64 conn_id, int dir, int opclass, void *buf, int len);
 * It locks, resolves conn_id to a connection, and queues buf[0..len) on the
 * channel dir selects. It returns >= 0 on success (bytes moved) and a negative
 * 0x8001xxxx LV2 error otherwise. */
typedef int (*shim_send_fn)(uint64_t conn_id, int dir, int opclass,
                            void *buf, int len);

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

/* F1 start-up for the DualSense in data-block slot `slot`: send the
 * feature-0x05 GET so the pad begins emitting 0x31 reports. It builds the PDU
 * {0x43, 0x05} in d->pdu - the data block's own buffer, never the driver's
 * shared one, which the lock this context does not hold protects - and calls the
 * transfer routine through d->send_routine as (d->conn_id[slot],
 * SHIM_SEND_DIR_CTRL_OUT, SHIM_SEND_OPCLASS_REPORT, d->pdu, SHIM_GET_PDU_LEN).
 * It does not read the reply: the send alone is what starts the stream (note
 * P2), and the running transition comes from the input wrapper translating those
 * reports. On a non-negative result it bumps stat_startups and returns F1's
 * running condition (SHIM_F1_OK); on a negative one it returns that error
 * unchanged.
 *
 * With no send routine, no connection id for the slot (0 - the plugin has not
 * recorded one) or a slot outside the table, it sends nothing and returns
 * SHIM_F1_ERR, so a DualSense start-up fails safe.
 *
 * Each send, accepted or not, spends one of the slot's SHIM_START_TRIES_MAX
 * (d->start_tries[slot]). Once they are spent it sends nothing and returns
 * SHIM_F1_OK - the value a successful start-up returns, so the driver keeps
 * polling the pad exactly as before. It does not fall through to the original
 * and does not return an error: either could cost the driver a pad that did
 * switch. The plugin re-arms a slot by zeroing its count (kshim_set_connection).
 *
 * arg1 is taken for signature fidelity with F1 - entry.c has already routed the
 * original's arg1==0 early-return to d->orig_startup, so the running transition
 * never depends on it here. */
int shim_startup_dualsense(ds45_shim_data_t *d, int slot, long arg1);

#endif /* DS45_SHIM_H */
