/*
 * DS45PAD kernel shim - the layout the plugin and the shim blob agree on.
 *
 * The plugin fills a ds45_shim_data_t in the blob's data area when it installs
 * the shim; the blob reads the configuration from it and the plugin reads the
 * counters back. The two sides are compiled separately (the plugin as a PRX,
 * the blob freestanding, and not necessarily with the same pointer size), so
 * this header holds fixed-width integers only, at their natural alignment, and
 * needs nothing but <stdint.h>.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_SHIM_ABI_H
#define DS45_SHIM_ABI_H

#include <stdint.h>

#define DS45_SHIM_MAGIC   0x4453343553484D31ULL   /* "DS45SHM1" */
/* 2: appended send_opd, conn_id, pdu and start_tries (the start-up
 * feature-0x05 GET), and get_report became send_routine. The installer adopts a
 * blob left by an earlier load only if its version matches, since the plugin
 * reaches into the data block by these offsets. */
#define DS45_SHIM_VERSION 2
#define DS45_SHIM_CONNS   8

typedef struct {
	uint8_t deadzone;              /* STICK_DEADZONE, percent of half travel */
	uint8_t map_touchpad, map_mute;/* ds45_map_t values */
	uint8_t sixaxis, swap_xy, invert_x, invert_y, invert_z, invert_g;
	int16_t accel_scale, gyro_scale;
} ds45_shim_cfg_t;

typedef struct {
	uint64_t magic; uint32_t version; uint32_t enabled;
	uint64_t orig_startup, orig_input;   /* original descriptors (F1, F2) */
	/* The address of the function descriptor the shim calls the kernel's
	 * Bluetooth HID transfer routine (note P1: int h116ef0(u64 conn_id, int dir,
	 * int opclass, void *buf, int len)) through; 0 = none. That routine has no
	 * descriptor of its own in the kernel, so the installer builds one in
	 * send_opd below and points this at it. It replaces the former get_report
	 * slot. */
	uint64_t send_routine;
	ds45_shim_cfg_t cfg;
	uint64_t conn_key[DS45_SHIM_CONNS];  /* per-connection identity (F1/F2) */
	uint8_t  conn_state[DS45_SHIM_CONNS];
	uint32_t stat_translated, stat_passed, stat_startups; /* debug counters */

	/* --- version 2: appended, so every offset above is unchanged --------- */
	uint64_t send_opd[3];                /* { entry, toc, env } of that routine, ELFv1; env 0 */
	/* The value the transfer routine takes as its connection id, per slot
	 * (valid when conn_state[i] != 0). The blob passes it through untouched;
	 * the plugin records it when the connection comes up (kshim_set_connection). */
	uint64_t conn_id[DS45_SHIM_CONNS];
	uint8_t  pdu[8];                     /* the request PDU the start-up builds and sends */
	/* How many times the start-up has sent its GET for each slot - written by
	 * the blob, one qword per slot so the plugin can zero one slot's count with
	 * a single aligned write and no read-modify-write of its neighbours. The
	 * driver may call the start-up again and again for one connection, so the
	 * blob stops sending when a slot's count reaches SHIM_START_TRIES_MAX
	 * (shim.h); kshim_set_connection zeroes it (re-arms the slot) before it
	 * publishes a connection. */
	uint64_t start_tries[DS45_SHIM_CONNS];
} ds45_shim_data_t;

#endif /* DS45_SHIM_ABI_H */
