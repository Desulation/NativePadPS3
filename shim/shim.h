/*
 * DS45PAD kernel shim - host-testable decision logic.
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
 * like ds3_report.c it includes only <stdint.h>, shim_abi.h, ds3_report.h and
 * shim.h, uses no libc and declares no static data.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_SHIM_H
#define DS45_SHIM_H

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
#define SHIM_PAD_INFO     0x80   /* the pad's DS3 info block (DS45_SHIM_INFO_LEN bytes) */
#define SHIM_PAD_XPORT    0x4F0  /* u64 pointer to the transport object */
#define SHIM_MGR_OBJS     0x60   /* pad manager: u64 object pointers */
#define SHIM_MGR_NOBJS    16
#define SHIM_ID_OFF       0x08   /* identity bytes in the info block: +0x08..+0x0F */

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
int shim_pad_is_dualsense(const ds45_shim_data_t *d, const uint8_t *pad);

/* Start-up event decision. For an enabled shim and a DualSense driver whose
 * substate != 9 and whose info pointer (drv+0x58) is exactly its pad + 0x80:
 * copy d->info_tmpl[0..DS45_SHIM_INFO_LEN) to the info block,
 * bump stat_completed and write the identity bytes info+0x08..+0x0F =
 * stat_completed (u32 big-endian), 0xD5, 0x00, 0x44, 0x53; store status 0
 * (4 bytes) and substate 9; return 1 (the caller returns 0 to the framework).
 * Otherwise write nothing and return 0 (the caller tail-calls the original).
 * drv NULL -> 0. */
int shim_event_complete(ds45_shim_data_t *d, uint8_t *drv);

/* Enable event decision. For an enabled shim and a DualSense whose enable
 * driver has drv[0x96] == 0: store status 0 (4 bytes) and drv[0x96] = 1, bump
 * stat_enabled, return 1. Otherwise write nothing and return 0. drv NULL -> 0. */
int shim_enable_complete(ds45_shim_data_t *d, uint8_t *drv);

/* The pad object whose +0x4F0 holds xport: mgr = *(u64 *)d->pad_mgr; scan the
 * SHIM_MGR_NOBJS u64 pointers at mgr+0x60, skipping nulls. NULL when d->pad_mgr
 * or mgr is 0, or no entry matches. Pure loads, at most 16 iterations. */
const uint8_t *shim_pad_from_xport(const ds45_shim_data_t *d, const void *xport);

/* Input filter, run after the original read has filled buf. For an enabled
 * DualSense whose buffer holds a basic report 0x01 or a full report 0x31 that
 * translates, buf is rewritten in place to a 49-byte DS3 report, *len becomes
 * 49 and stat_translated is bumped; otherwise buf and *len are left untouched
 * and stat_passed is bumped. *len carries the original byte count in and the
 * resulting count out (the read's length is a half-word, so uint16_t), and the
 * return value is the resulting count (the read's return value). buf must have
 * room for DS3_REPORT_LEN bytes. d->enabled == 0 forces passthrough. */
int shim_input_filter(ds45_shim_data_t *d, uint16_t vid, uint16_t pid,
                      uint8_t *buf, uint16_t *len);

#endif /* DS45_SHIM_H */
