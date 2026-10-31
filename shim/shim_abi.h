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
#define DS45_SHIM_VERSION 1
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
	uint64_t get_report;                 /* F3 helper descriptor */
	ds45_shim_cfg_t cfg;
	uint64_t conn_key[DS45_SHIM_CONNS];  /* per-connection identity (F1/F2) */
	uint8_t  conn_state[DS45_SHIM_CONNS];
	uint32_t stat_translated, stat_passed, stat_startups; /* debug counters */
} ds45_shim_data_t;

#endif /* DS45_SHIM_ABI_H */
