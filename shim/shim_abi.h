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
/* 3: the start-up completion layout (event and enable hooks, pad identity, the
 * DS3 info template); every v2 connection, send and start-up field is gone. The
 * installer adopts a blob left by an earlier load only if its version matches,
 * since the plugin reaches into the data block by these offsets. */
#define DS45_SHIM_VERSION 3
/* The DS3 info block the start-up completion copies to pad+0x80 .. +0xD5
 * (facts note C1.5). info_tmpl holds it, rounded up to a whole qword. */
#define DS45_SHIM_INFO_LEN 0x56
/* Product-ID sites a row can edit: the helper word and the two inline words
 * (src/kshim.h: DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES, checked there). */
#define DS45_SHIM_PID_SITES 3

typedef struct {
	uint8_t deadzone;              /* STICK_DEADZONE, percent of half travel */
	uint8_t map_touchpad, map_mute;/* ds45_map_t values */
	uint8_t sixaxis, swap_xy, invert_x, invert_y, invert_z, invert_g;
	int16_t accel_scale, gyro_scale;
} ds45_shim_cfg_t;

typedef struct {
	uint64_t magic; uint32_t version; uint32_t enabled;
	/* The kernel's original descriptors the hooks tail-call: the start-up
	 * driver's event handler (slot 0x37CFF8), the enable driver's event handler
	 * (slot 0x37CFB0) and the Bluetooth transport's input read (slot 0x37CE38). */
	uint64_t orig_event, orig_enable, orig_input;
	uint64_t pad_vtable;           /* the Sony pad class: 0x800000000038CDC8 on DEX 4.92 */
	uint64_t pad_mgr;              /* address of the pad-manager pointer: 0x80000000004A1CB8 */
	ds45_shim_cfg_t cfg;
	/* Debug counters, written by the blob. stat_completed also numbers the
	 * DualSense identities: the first DualSense of a boot gets 1. */
	uint32_t stat_completed;
	uint32_t stat_enabled, stat_translated, stat_passed;
	/* The DS3 info template; the first DS45_SHIM_INFO_LEN bytes are used and its
	 * identity bytes (index 0x08..0x0F) are zero - the blob writes them per pad. */
	uint8_t  info_tmpl[0x58];
	/* Written by the plugin only, appended so every offset above is kept. The
	 * original 8-aligned qword of each product-ID site, read at install before
	 * the word edit, and which sites the install applied (bit i = helper patch
	 * i, bit 1 + j = inline patch j). A later load that adopts this blob rebuilds
	 * its journal from them, so its unload switches the word edits back too.
	 * Kernel RAM only, never in git. The blob never reads them. */
	uint64_t pid_orig[DS45_SHIM_PID_SITES];
	uint32_t pid_applied;
	uint32_t pid_reserved;         /* keeps the size a multiple of 8 */
} ds45_shim_data_t;

#endif /* DS45_SHIM_ABI_H */
