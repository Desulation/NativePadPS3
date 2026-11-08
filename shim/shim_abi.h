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
 * 272 -> 584.
 * 9: the rumble-aware output capture (debug only) appended after pktbuf:
 * 584 -> 744.
 * 10: the receive-limit words (sites 7-9) grow pid_orig from 7 to 10 sites, which
 * moves every field after it by 24 bytes (744 -> 768), and the input bounce buffer
 * (inbuf, 96 bytes) and its four debug counters are appended last: 768 -> 880.
 * 11: the raw DualSense report capture (debug only) appended after the bounce
 * counters: 880 -> 952.
 * 12: the power-off hook's state appended after dbg_in_raw (the two original code
 * addresses, mode and sequence, debug counters, a 48-byte frame buffer and an
 * 8 x 16 byte ring: 224 bytes), and pid_orig grown from 10 to 14 sites (sites 10-13
 * are the power-off patches), which moves every field after it by 32 bytes
 * (952 -> 984, then 984 + 224 = 1208). */
#define DS45_SHIM_VERSION 12
/* The DS3 info block the start-up completion copies to pad+0x80 .. +0xD5
 * (facts note C1.5). info_tmpl holds it, rounded up to a whole qword. */
#define DS45_SHIM_INFO_LEN 0x56
/* Product-ID sites a row can edit, in data-block order: 0 the helper branch, 1-2
 * the inline words, then the output sites - 3 the output call site, 4 the sender's
 * payload-cap word, 5 the TOC slot holding the packet buffer's address, 6 the TOC
 * slot holding that address plus one, then the receive-limit words - 7, 8 and 9,
 * the three `li r7,0x41` immediates (the kernel's read limit, 0x41 bytes) that
 * become `li r7,0x50`, so the DualSense's 79-byte full report fits (src/kshim.h:
 * DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + DS45_SHIM_OUTPUT_PATCHES +
 * DS45_SHIM_RECV_PATCHES + DS45_SHIM_POWEROFF_PATCHES, checked there), then the
 * power-off patches - 10 to 13, the rows of the power-off hook (version 12; not
 * selected by any install yet). */
#define DS45_SHIM_PID_SITES 14

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
	/* Rumble-aware capture, written by the blob, debug only (version 9). A payload
	 * is "motor on" when it is longer than 4 bytes and payload byte 2 (small motor)
	 * or byte 4 (large motor) is non-zero. dbg_out_on_* hold the last motor-on
	 * payload from any pad (count, length, bytes, and that pad's address, the
	 * payload address minus 0x22B). dbg_out_off_* hold the first zero-motor payload
	 * (longer than 4 bytes, bytes 2 and 4 zero) from that same pad after it;
	 * dbg_out_off_armed is 1 from a motor-on payload until that capture is taken.
	 * dbg_out_rsvd9 keeps the size a multiple of 8. */
	uint64_t dbg_out_on_pad;
	uint32_t dbg_out_on_count, dbg_out_on_len, dbg_out_off_count, dbg_out_off_len;
	uint32_t dbg_out_off_armed, dbg_out_rsvd9;
	uint8_t  dbg_out_on_cap[64];
	uint8_t  dbg_out_off_cap[64];
	/* The input bounce buffer (version 10): with the receive-limit words raised, a
	 * DualSense sends 79-byte full reports, which the input hook reads into inbuf
	 * (8-aligned; zero at install) and translates for the caller. The dbg_in_*
	 * counters are written by shim_input_bounce, debug only: every bounce read (any
	 * pad: in the output modes 1 and 2 every read of the input hook is bounced, not
	 * just a DualSense's), reads of 66-79 bytes, results dropped for not fitting
	 * the caller's buffer, and errors. The installer leaves them zero. The size is
	 * kept a multiple of 8. */
	uint8_t  inbuf[96];
	uint32_t dbg_in_bounce, dbg_in_big, dbg_in_drop, dbg_in_err;
	/* The raw DualSense report capture (version 11), written by the blob, debug
	 * only: the first dbg_in_raw_len (at most 64) bytes of the last DualSense read
	 * of 1..0x4F bytes, copied BEFORE the in-place translation, and dbg_in_raw_seq,
	 * which counts those captures. The plugin's trace prints them when the sequence
	 * changed, to show what the pad really sends. The size is kept a multiple of 8. */
	uint32_t dbg_in_raw_len, dbg_in_raw_seq;
	uint8_t  dbg_in_raw[64];
	/* The power-off hook's state (version 12), appended after dbg_in_raw.
	 * orig_poweroff and orig_teardown are the code addresses of the kernel's
	 * Bluetooth close-connection sender (0x102750 on DEX 4.92) and of the pad
	 * teardown routine (0x1174FC), which the hooks tail-call; written by the plugin
	 * from the firmware row, 0 = none. po_mode is the power-off stage, written by the
	 * plugin; po_seq is the report sequence counter, written by the blob. The dbg_po_*
	 * and dbg_td_* counters are written by the blob, debug only: how often the close
	 * hook was entered, how many calls were for a DualSense, how many it rewrote, how
	 * many it passed through unchanged; how often the teardown hook was entered and
	 * (dbg_td_rsvd) a reserved word that keeps the size a multiple of 8. po_buf is the
	 * 48-byte report the hook builds (8-aligned; zero at install). po_ring is a ring
	 * of eight 16-byte records of the last calls, debug only, never read by the
	 * plugin; a record is:
	 *   bytes 0-3   sequence (big-endian)
	 *   byte 4      site: 1 = close send, 2 = teardown
	 * for site 1:
	 *   byte 5      DualSense flag
	 *   byte 6      connection handle
	 *   byte 7      r4
	 *   byte 8      report id
	 *   byte 9      length
	 *   byte 10     kind (0xFF if unreadable; the LOW BYTE of the 32-bit kind, so
	 *               a kind with low byte 0xFF also reads 0xFF here)
	 *   byte 11     zero
	 *   bytes 12-15 the first four payload bytes (zero-padded)
	 * for site 2:
	 *   byte 5      zero
	 *   byte 6      flag
	 *   byte 7      zero
	 *   bytes 8-11  low 32 bits of LR
	 *   bytes 12-15 low 32 bits of the address argument
	 * The installer leaves all of it zero except the two addresses. */
	uint64_t orig_poweroff, orig_teardown;
	uint32_t po_mode, po_seq;
	uint32_t dbg_po_calls, dbg_po_ds, dbg_po_rewritten, dbg_po_passed;
	uint32_t dbg_td_calls, dbg_td_rsvd;
	uint8_t  po_buf[48];
	uint8_t  po_ring[8][16];
} ds45_shim_data_t;

#endif /* DS45_SHIM_ABI_H */
