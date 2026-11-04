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
/* 6: stage 1b's stable identity appended after the debug counters (conn_tbl,
 * stable_id and three counters: 248 -> 272 bytes). 5: the debug diagnostic counters (design Amendment 3) appended after the
 * product-ID originals (208 -> 248 bytes). 4 added the product-ID originals
 * (pid_orig, pid_applied: 176 -> 208); 3 was the start-up completion layout
 * without them and never reached a console. The installer adopts a blob left by
 * an earlier load only if its version matches, since the plugin reaches into the
 * data block by these offsets - so any change to this struct bumps the number.
 * 7: the helper product-ID row became a branch to shim_pid_ok (same layout), so a
 * build that applied the old word edit is not adopted.
 * 8: the DS3-route output state (rumble, player LEDs, light bar) appended, and
 * pid_orig grown from 3 to 7 sites, which moves every field after it by 24 bytes:
 * 272 -> 584. */
#define DS45_SHIM_VERSION 8
/* The DS3 info block the start-up completion copies to pad+0x80 .. +0xD5
 * (facts note C1.5). info_tmpl holds it, rounded up to a whole qword. */
#define DS45_SHIM_INFO_LEN 0x56
/* Product-ID sites a row can edit, in data-block order: 0 the helper branch, 1-2
 * the inline words, then the output sites - 3 the output call site, 4 the sender's
 * payload-cap word, 5 the TOC slot holding the packet buffer's address, 6 the TOC
 * slot holding that address plus one (src/kshim.h: DS45_SHIM_PID_PATCHES +
 * DS45_SHIM_INLINE_PATCHES + DS45_SHIM_OUTPUT_PATCHES, checked there). */
#define DS45_SHIM_PID_SITES 7

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
	 * DualSense identities: the first DualSense of a boot gets 1. It doubles as
	 * the identity counter, so it must NEVER be reset (not by the plugin, not on
	 * adoption, not to clear the stats): a reset would hand a later DualSense an
	 * identity already in use this boot. stat_passed counts every read not
	 * translated, a DS3's included (shim.c). */
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
	/* Debug diagnostic counters (design Amendment 3), written lock-free by the
	 * blob exactly like the stat_* counters and read back by the plugin's debug
	 * housekeeping (never on the console's shipping path; the game build installs
	 * no shim). Per hook: how many times it was entered (dbg_*_calls) and matched
	 * the DualSense identity (dbg_*_ds); the input hook's pad-lookup misses
	 * (dbg_input_padmiss); and the last VID/PID (big-endian VID||PID) and pad
	 * vtable the input hook identified. They answer "did the hooks fire for the
	 * DS3, and what did they see". */
	uint32_t dbg_event_calls, dbg_enable_calls, dbg_input_calls, dbg_input_padmiss;
	uint32_t dbg_event_ds, dbg_enable_ds, dbg_input_ds, dbg_last_pid;
	uint64_t dbg_last_vtable;
	/* Stable pad identity (stage 1b). conn_tbl is the address of the kernel's
	 * Bluetooth connection table (an array of 7 u64 entry pointers indexed by the
	 * connection handle; 0 = not available). Written by the plugin from the
	 * firmware row. stable_id (the SHIM_STABLE_ID config key, 0/1) asks the start-up
	 * hook to take a DualSense's identity from its connection's Bluetooth address
	 * instead of the per-boot counter; the counter stays the fallback. The three
	 * counters are written by the blob, debug only: identities taken from an
	 * address, completions that fell back to the counter, and the last handle
	 * looked up. */
	uint64_t conn_tbl;
	uint32_t stable_id;
	uint32_t dbg_id_bd, dbg_id_fallback, dbg_last_handle;
	/* DS3-route output (rumble, player LEDs, light bar). orig_output is the code
	 * address of the kernel's Bluetooth output sender (0x10283C on DEX 4.92), which
	 * the output hook tail-calls; written by the plugin from the firmware row, 0 =
	 * none. out_mode (the SHIM_OUTPUT_MODE config key, 0..2: observe, probe, full)
	 * is written by the plugin. out_seq is the DualSense report sequence counter,
	 * written by the blob. The dbg_out_* counters are written by the blob, debug
	 * only: how often the hook was entered, how many calls were for a DualSense,
	 * how many it rewrote, how many it passed through unchanged, the length of the
	 * last call and the length of the payload captured into dbg_out_cap. out_buf is
	 * the 78-byte DualSense report the hook builds and pktbuf the moved Bluetooth
	 * packet buffer (8-aligned; zero at install); neither is read by the plugin.
	 * The size is kept a multiple of 8. */
	uint64_t orig_output;
	uint32_t out_mode, out_seq;
	uint32_t dbg_out_calls, dbg_out_ds, dbg_out_translated, dbg_out_passed;
	uint32_t dbg_out_last_len, dbg_out_cap_len;
	uint8_t  dbg_out_cap[64];
	uint8_t  out_buf[80];
	uint8_t  pktbuf[96];
} ds45_shim_data_t;

#endif /* DS45_SHIM_ABI_H */
