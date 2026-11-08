/*
 * NPP kernel shim - host-testable decision logic.
 *
 * The wrappers the plugin points the kernel's DS3 driver at (the start-up
 * driver's event handler, the enable driver's event handler and the Bluetooth
 * input read) split into two layers:
 *
 *   - this file / shim.c: the decisions, taking the data block and the kernel
 *     objects (driver, pad, buffer) explicitly, so they are pure and
 *     host-testable with no kernel calls;
 *   - shim/entry.c: the thin kernel-ABI glue that hands the hook arguments to
 *     the functions here and tail-calls the original for anything that is not
 *     an enabled DualSense.
 *
 * The decisions only store into the driver object and its pad's info block,
 * which the original handlers already write on this same thread: no sends, no
 * locks, no allocation, no calls into the kernel, bounded straight-line code.
 *
 * It compiles INTO the freestanding kernel blob as well as the host tests, so
 * like ds3_report.c it includes only <stdint.h>, shim_abi.h, ds3_report.h,
 * ds3_output.h and shim.h, uses no libc and declares no static data.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef NPP_SHIM_H
#define NPP_SHIM_H

#include <stdint.h>
#include "shim_abi.h"

/* Kernel object offsets (DEX 4.92, ledger "START-UP DRIVER MAPPED", facts note C1-C3). */
#define SHIM_DRV_PAD      0x48   /* u64 pad object (both drivers) */
#define SHIM_DRV_STATUS   0x50   /* u32 status passed to the callback (both drivers) */
#define SHIM_DRV_SUBSTATE 0x54   /* u8, start-up driver: 9 = done */
#define SHIM_DRV_INFO     0x58   /* u64 -> pad+0x80, start-up driver */
#define SHIM_DRV_DONE     9
#define SHIM_EN_DONE      0x96   /* u8, enable driver: 1 = done */
#define SHIM_PAD_ID       0x68   /* big-endian VID then PID */
#define SHIM_PAD_INFO     0x80   /* the pad's DS3 info block (NPP_SHIM_INFO_LEN bytes) */
#define SHIM_PAD_XPORT    0x4F0  /* u64 pointer to the transport object */
#define SHIM_MGR_OBJS     0x60   /* pad manager: u64 object pointers */
#define SHIM_MGR_NOBJS    16
#define SHIM_ID_OFF       0x08   /* identity bytes in the info block: +0x08..+0x0F */
#define SHIM_DRV_HANDLE   0xAC   /* u8, start-up driver: the Bluetooth connection handle */
#define SHIM_CONN_NHANDLES 7     /* connection table: handles 0..6 */
#define SHIM_CONN_HANDLE  0x10   /* u8 in a connection entry: its own handle */
#define SHIM_CONN_ID      0x08   /* a connection entry's +0x08..+0x0F: the controller's
                                  * Bluetooth address in the pad-identity layout */
#define SHIM_PAD_OUT_BUF_OFF 0x22B /* the pad's output payload buffer: the kernel's output
                                  * call (0x2A2DE8) passes pad + 0x22B as the payload, so
                                  * pad = payload - 0x22B (facts note F6) */
#define SHIM_OUT_MAX_LEN  64     /* the kernel sender's payload cap (also the size of
                                  * the data block's dbg_out_cap) */

/* 1 only for a DualSense (VID 0x054C, PID 0x0CE6). */
int shim_is_dualsense(uint16_t vid, uint16_t pid);

/* Product-ID helper, kept for stage 1b (nothing branches to it in stage 1).
 * 1 for VID 0x054C with PID 0x0268, 0x042F or 0x0CE6, else 0. It is meant to be
 * called by the kernel by its raw code address with the kernel's own r2, so it
 * must stay a leaf that touches no global and no TOC (tools/shim_pack.py
 * enforces that). */
int shim_pid_ok(uint32_t vid, uint32_t pid);

/* 1 if pad is a live Sony pad object (u64 at pad+0 == d->pad_vtable) whose
 * pad+0x68 reads 054C/0CE6, else 0. NULL pad -> 0. */
int shim_pad_is_dualsense(const npp_shim_data_t *d, const uint8_t *pad);

/* Start-up event decision. For an enabled shim and a DualSense driver whose
 * substate != 9 and whose info pointer (drv+0x58) is exactly its pad + 0x80:
 * copy d->info_tmpl[0..NPP_SHIM_INFO_LEN) to the info block,
 * bump stat_completed and write the identity bytes info+0x08..+0x0F; store
 * status 0 (4 bytes) and substate 9; return 1 (the caller returns 0 to the
 * framework).
 *
 * The identity is, when d->stable_id is set and d->conn_tbl is not 0, the
 * controller's own Bluetooth address: h = drv[0xAC] (the connection handle),
 * entry = the u64 at conn_tbl + 8*h, and info+0x08..+0x0F = entry+0x08..+0x0F with
 * the unused byte +0x0D forced to 0. It is taken only if h < 7, entry is non-NULL,
 * entry[0x10] == h, the address is not all zero and is not the kernel's
 * alternate-branch pattern (info+0x0C == 0x18 with the u16 at +0x0E == 0x000D);
 * otherwise (and when stable_id is 0 or conn_tbl is 0) the identity is the
 * per-boot counter form stat_completed (u32 big-endian), 0xD5, 0x00, 0x44, 0x53.
 * dbg_id_bd counts addresses taken, dbg_id_fallback counts attempts that fell
 * back, dbg_last_handle records the handle looked up. Pure loads.
 * Otherwise write nothing and return 0 (the caller tail-calls the original).
 * drv NULL -> 0. */
int shim_event_complete(npp_shim_data_t *d, uint8_t *drv);

/* Enable event decision. For an enabled shim and a DualSense whose enable
 * driver has drv[0x96] == 0: store status 0 (4 bytes) and drv[0x96] = 1, bump
 * stat_enabled, return 1. Otherwise write nothing and return 0. drv NULL -> 0. */
int shim_enable_complete(npp_shim_data_t *d, uint8_t *drv);

/* Debug tracing (design Amendment 3): record what the input hook saw into the
 * data block's diagnostic counters. Always bumps dbg_input_calls; a NULL pad
 * bumps dbg_input_padmiss and returns; otherwise records dbg_last_vtable (the
 * pad's class qword at pad+0) and dbg_last_pid (the big-endian VID||PID at
 * pad+0x68), and bumps dbg_input_ds when the pad is a DualSense. NULL d is a
 * no-op. Pure loads and lock-free counter bumps, like the stat_* counters. */
void shim_input_note(npp_shim_data_t *d, const uint8_t *pad);

/* The pad object whose +0x4F0 holds xport: mgr = *(u64 *)d->pad_mgr; scan the
 * SHIM_MGR_NOBJS u64 pointers at mgr+0x60, skipping nulls. NULL when d->pad_mgr
 * or mgr is 0, or no entry matches. Pure loads, at most 16 iterations. */
const uint8_t *shim_pad_from_xport(const npp_shim_data_t *d, const void *xport);

/* Input filter, run after the original read has filled buf. For an enabled
 * DualSense whose buffer holds a basic report 0x01 or a full report 0x31 that
 * translates, buf is rewritten in place to a 49-byte DS3 report, *len becomes
 * 49 and stat_translated is bumped; otherwise buf and *len are left untouched
 * and stat_passed is bumped. *len carries the original byte count in and the
 * resulting count out (the read's length is a half-word, so uint16_t), and the
 * return value is the resulting count (the read's return value). buf must have
 * room for DS3_REPORT_LEN bytes. d->enabled == 0 forces passthrough. */
int shim_input_filter(npp_shim_data_t *d, uint16_t vid, uint16_t pid,
                      uint8_t *buf, uint16_t *len);

/* The bounce read is given this length (the raised read limit: the transport
 * returns at most 0x4F, the packet minus its header byte). */
#define SHIM_IN_BOUNCE_LEN 0x50
#define SHIM_IN_BOUNCE_MAX (SHIM_IN_BOUNCE_LEN - 1)

/* Bounce-buffer input decision (full-report input). The input hook, for an
 * enabled shim in output mode 1 or 2, bounces EVERY read, whichever pad it is:
 * it calls the original read with d->inbuf (96 bytes, shim-owned) and a length
 * of 0x50 instead of the caller's dst, because the transport read copies the
 * whole packet, up to 78 bytes for a full 0x31 report, with no bound, the raised
 * limit is shared by every device on the channel, and the caller's buffer is 64
 * bytes. rc is that read's result; pad is the pad found for the transport AFTER
 * the read (NULL when the lookup missed or was skipped); dst/len_in are the
 * caller's buffer. Only a DualSense's report is translated; any other pad's
 * bytes pass through unchanged. It returns the value the hook returns to the
 * kernel:
 *   - d NULL or dst NULL: nothing is touched; rc if rc < 0, else 0.
 *   - dbg_in_bounce++ for every bounce read, any pad.
 *   - rc < 0: dbg_in_err++, return rc (nothing else is counted or written).
 *   - otherwise shim_input_note(d, pad); then rc == 0: return 0;
 *     rc > 0x4F (cannot happen, the read is limited to 0x4F): dbg_in_drop++,
 *     return 0; rc > 0x41: dbg_in_big++.
 *   - Raw capture (version 11, debug only): for a DualSense pad, BEFORE the filter
 *     below rewrites inbuf, min(rc, 64) bytes of d->inbuf are copied to
 *     d->dbg_in_raw, dbg_in_raw_len is set to that count and dbg_in_raw_seq++.
 *     Nothing is captured for any other pad or for rc <= 0 or rc > 0x4F.
 *   - n = rc; shim_input_filter runs on d->inbuf: with the DualSense identity
 *     (0x054C, 0x0CE6) when pad is a DualSense, where it translates a basic 0x01
 *     or full 0x31 report in place into the 49-byte DS3 report (n becomes 49)
 *     and otherwise leaves it; with identity 0 for any other pad (a DS3, an
 *     unmatched pad, pad_mgr or pad_vtable 0), where it never translates
 *     (n = rc, stat_passed++ as on the normal path).
 *   - n <= len_in: exactly n bytes are copied from d->inbuf to dst and n is
 *     returned. n > len_in (a long record the caller cannot hold, or a
 *     translated report and len_in < 49): dbg_in_drop++, return 0, dst untouched.
 * dst is never written past n <= len_in bytes, for any pad, rc and len_in. Pure
 * loads and stores, a plain bounded byte loop (no memcpy), no other call than
 * the helpers above. */
int shim_input_bounce(npp_shim_data_t *d, const uint8_t *pad, uint8_t *dst,
                      uint16_t len_in, int rc);

/* Output decision (DS3-route output), run from the stub on the kernel's output
 * call (the Bluetooth sender 0x10283C): buf is the payload (pad + 0x22B,
 * SHIM_PAD_OUT_BUF_OFF), len its length (the kernel caps it at 64). Returns 1
 * when the call must be rewritten, in which case *rid, *out_buf and *out_len
 * carry the replacement report id, payload pointer and length; 0 to pass the
 * call through unchanged, and then none of the three out-parameters is written.
 *
 * Rewriting (after the counting and capture below, which never change): only
 * when d->out_mode is 1 or 2 and the pad (buf - 0x22B) is a DualSense, and the
 * three out-parameters are non-NULL. The 78-byte DualSense report is built in
 * d->out_buf with sequence number d->out_seq - mode 1 (probe):
 * ds3_output_probe, mode 2 (full): ds3_output_to_dualsense of the payload (a
 * refusal, -1, passes the call through, out_seq not advanced). On success
 * out_seq = (out_seq + 1) & 0x0F, *rid = 0x31, *out_buf = d->out_buf + 1 (the
 * report without its id, which the sender takes separately), *out_len = 77,
 * dbg_out_translated++ (dbg_out_passed is not bumped), return 1. Mode 0, any
 * other mode, a DS3, a non-Sony or unknown pad and a disabled shim never
 * rewrite.
 *
 * It always counts and records, for any pad (the capture of what the console
 * really sends a DualShock 3 is the ground truth the rewrite is built on):
 *   - d NULL: return 0, write nothing.
 *   - otherwise dbg_out_calls++ first. Then, if the shim is disabled or buf is
 *     NULL or len is 0 or over 64: return 0 (counters only, no capture).
 *   - otherwise dbg_out_last_len = len, the payload's min(len, 64) bytes are
 *     copied to dbg_out_cap with dbg_out_cap_len set to that count, the pad
 *     (buf - 0x22B) is tested with shim_pad_is_dualsense (dbg_out_ds++ for a
 *     DualSense), dbg_out_passed++ and 0 is returned.
 * Only loads from the buffer and the pad, stores into the data block, a plain
 * bounded byte loop (no memcpy), no lock, no call. */
int shim_output_filter(npp_shim_data_t *d, const uint8_t *buf, uint16_t len,
                       uint8_t *rid, const uint8_t **out_buf, uint16_t *out_len);

/* Power-off hook (DS3-route power-off; the kernel facts are in
 * docs/superpowers/notes/2026-11-04-ds3-route-poweroff-gate.md). The stub on the
 * kernel's Bluetooth close-connection sender (0x102750 on DEX 4.92, called as
 * (handle, type, report id, payload, length, cookie, obj)) hands the registers
 * r3..r9 to shim_poweroff_filter in a npp_po_call_t, plus three replacement
 * values the filter may fill; on a return of 1 the stub loads r5, r6 and r7 from
 * o_rid, o_buf and o_len and tail-calls the original, on 0 it leaves the
 * registers alone. */
typedef struct {
	uint64_t handle, type, rid, payload, len, cookie, obj;
	uint64_t o_rid, o_buf, o_len;
} npp_po_call_t;

/* Stack frame layout of the shim_poweroff and shim_teardown stubs (shim/entry.c),
 * fixed here so the host test can pin them against the call record. Both frames are
 * 16-byte aligned, keep the back chain (stdu) and a 0x30..0x6F parameter save area
 * (eight doublewords) for the C body they call; nothing above the frame is touched.
 *   power-off, frame SHIM_PO_FRAME: the npp_po_call_t at SHIM_PO_CALL (the stub
 *     stores r3..r9 into its first seven fields, the body writes o_*), then the
 *     saved LR and the saved kernel r2.
 *   teardown, frame SHIM_TD_FRAME: saved LR, saved kernel r2, then r3..r10. */
#define SHIM_STUB_PARAM_LO 0x30
#define SHIM_STUB_PARAM_HI 0x70  /* end of the parameter save area (exclusive) */
#define SHIM_PO_FRAME      0xD0
#define SHIM_PO_CALL       0x70
#define SHIM_PO_SAVED_LR   0xC0
#define SHIM_PO_SAVED_R2   0xC8
#define SHIM_TD_FRAME      0xC0
#define SHIM_TD_SAVED_LR   0x70
#define SHIM_TD_SAVED_R2   0x78
#define SHIM_TD_ARGS       0x80  /* r3..r10, eight doublewords */

#define SHIM_PO_OBJ_KIND   0x38  /* u32 in the call's object: the kind of request */
#define SHIM_PO_OBJ_PAD    0x48  /* u64 in the call's object: the pad */
#define SHIM_PO_KIND_CLOSE 3     /* the kind of the Bluetooth close (power-off) request */

/* Power-off decision. d or c NULL: return 0, write nothing. A disabled shim
 * (d->enabled == 0): return 0 before anything is counted or recorded (like
 * shim_teardown_note). Otherwise, in every po_mode:
 *   - dbg_po_calls++; the kind (u32 at obj+0x38) and the pad (u64 at obj+0x48) are
 *     read only when c->obj is non-NULL (kind 0xFF in the ring, and no pad,
 *     otherwise); is_ds = shim_pad_is_dualsense(d, pad) (dbg_po_ds++ when set);
 *   - a site-1 ring record is appended (po_seq++ first, slot (po_seq - 1) & 7,
 *     the layout of shim_abi.h; the first min(len, 4) payload bytes are read only
 *     when the payload pointer is non-NULL).
 * Rewrite - only when d->po_mode == 1, c->type == 3, c->rid == 0xF4, c->len == 4,
 * the kind is SHIM_PO_KIND_CLOSE and the pad is a DualSense: d->po_buf is filled
 * with ds_poweroff_payload (46 bytes; the 2 bytes after stay zero), c->o_rid =
 * 0x08, c->o_buf = d->po_buf, c->o_len = 46, dbg_po_rewritten++, return 1. Every
 * other path: dbg_po_passed++, return 0, o_rid, o_buf and o_len untouched. The
 * call's own fields are never written. The same code is reached with other
 * shapes (type 2, rid 1, ...): those pass. Only loads from the call's objects,
 * stores into the data block, plain bounded byte loops, no lock, no call but
 * the pure helpers. */
int shim_poweroff_filter(npp_shim_data_t *d, npp_po_call_t *c);

/* Teardown observation (the pad teardown routine 0x1174FC(address, flag), called
 * with link register lr). d NULL or disabled: nothing. Otherwise dbg_td_calls++
 * and a site-2 ring record (po_seq++ first; byte 6 the low byte of flag, bytes
 * 8-11 the low 32 bits of lr, bytes 12-15 the low 32 bits of addr, big-endian).
 * It returns nothing and changes no argument: the stub tail-calls the original
 * with its registers untouched. */
void shim_teardown_note(npp_shim_data_t *d, uint64_t addr, uint64_t flag, uint64_t lr);

#endif /* NPP_SHIM_H */
