/*
 * NPP - Full DualShock 4 / DualSense compatibility plugin for PlayStation 3
 *
 * Shared types, constants and module-wide declarations.
 *
 * The plugin claims a DS4/DualSense over USB (or reads the OEM Bluetooth pad in
 * passthrough mode), translates its HID reports into a DualShock 3 shaped
 * CellPadData and injects it through the libpad LDD (Logical Device Driver)
 * interface as a controller that advertises full PS3 conformity.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef NPP_H
#define NPP_H

#include <stdint.h>
#include <stddef.h>
#include <cell/pad.h>   /* CellPadData in the VSH detour prototypes below */
#include "report_queue.h"
#include "npp_state.h" /* npp_state_t, NPP_B_*, npp_map_t, DualSense layout */
#include "sixaxis.h"    /* NPP_SENSOR_CENTER, sixaxis_from_accel/gyro */

#ifdef NPP_DEBUG
#define NPP_VERSION "1.0.1-ps-usb-candidate-debug"
#else
#define NPP_VERSION "1.0.1-ps-usb-candidate"
#endif

/* The LV2 dumper (lv2dump.c): debug builds of the VSH plugin only. Nothing of
 * it - code, config key, file names, toast - exists in release or game builds. */
#if defined(NPP_DEBUG) && !defined(NPP_GAME)
#define NPP_LV2DUMP 1
#endif

/* Where the plugin looks for its data files (paths.c). The folder is chosen
 * once, by npp_paths_init() at plugin start:
 *   1. NPP_DATADIR_NEW, if its config file exists;
 *   2. else the legacy folder (old name, old file names), if its config exists;
 *   3. else NPP_DATADIR_NEW.
 * npp_path() never probes and returns the new folder's file before init; an
 * out-of-range kind gives "". */
#define NPP_DATADIR_NEW "/dev_hdd0/plugins/nativepadps3"
typedef enum {
	NPP_PATH_CONFIG = 0,
	NPP_PATH_DEVICES,
	NPP_PATH_CAL,
	NPP_PATH_KERNEL_CACHE,
	NPP_PATH_COUNT
} npp_path_kind_t;
void        npp_paths_init(void);
const char *npp_path(npp_path_kind_t kind);

#define NPP_DIAG_FILE   "/dev_hdd0/tmp/nativepadps3_diag.txt"
#define NPP_BOOT_FILE    "/dev_hdd0/tmp/nativepadps3_boot.txt"

/* ------------------------------------------------------------------------ */
/* libpad constants                                                          */
/*                                                                           */
/* These come from <cell/pad.h> in the official SDK. They are re-declared    */
/* here behind #ifndef so the translation layer can be read (and unit        */
/* checked) without the proprietary headers, and so a missing/renamed SDK    */
/* symbol produces a clean value instead of a build break.                   */
/* Values verified against RPCS3's rpcs3/Emu/Io/pad_types.h.                 */
/* ------------------------------------------------------------------------ */

/* Deliberately not named CELL_PAD_MAX_PORT_NUM: the SDK may declare that as
 * an enumerator rather than a macro, and shadowing it with a #define would
 * depend on header include order to stay harmless. */
#define NPP_MAX_PORT_NUM       7

/* Indices into CellPadData.button[] */
#define NPP_BTN_LDD            0   /* LDD-only extension word          */
#define NPP_BTN_DIGITAL1       2
#define NPP_BTN_DIGITAL2       3
#define NPP_BTN_ANALOG_RIGHT_X 4
#define NPP_BTN_ANALOG_RIGHT_Y 5
#define NPP_BTN_ANALOG_LEFT_X  6
#define NPP_BTN_ANALOG_LEFT_Y  7
#define NPP_BTN_PRESS_RIGHT    8
#define NPP_BTN_PRESS_LEFT     9
#define NPP_BTN_PRESS_UP       10
#define NPP_BTN_PRESS_DOWN     11
#define NPP_BTN_PRESS_TRIANGLE 12
#define NPP_BTN_PRESS_CIRCLE   13
#define NPP_BTN_PRESS_CROSS    14
#define NPP_BTN_PRESS_SQUARE   15
#define NPP_BTN_PRESS_L1       16
#define NPP_BTN_PRESS_R1       17
#define NPP_BTN_PRESS_L2       18
#define NPP_BTN_PRESS_R2       19
#define NPP_BTN_SENSOR_X       20
#define NPP_BTN_SENSOR_Y       21
#define NPP_BTN_SENSOR_Z       22
#define NPP_BTN_SENSOR_G       23

/* A DS3 in press+sensor mode reports 24 codes. */
#define NPP_PAD_LEN            24

/* button[NPP_BTN_DIGITAL1] bits */
#define NPP_CTRL_SELECT        0x0001
#define NPP_CTRL_L3            0x0002
#define NPP_CTRL_R3            0x0004
#define NPP_CTRL_START         0x0008
#define NPP_CTRL_UP            0x0010
#define NPP_CTRL_RIGHT         0x0020
#define NPP_CTRL_DOWN          0x0040
#define NPP_CTRL_LEFT          0x0080
#define NPP_CTRL_PS            0x0100  /* PS button, digital1 extension */

/* button[NPP_BTN_DIGITAL2] bits */
#define NPP_CTRL_L2            0x0001
#define NPP_CTRL_R2            0x0002
#define NPP_CTRL_L1            0x0004
#define NPP_CTRL_R1            0x0008
#define NPP_CTRL_TRIANGLE      0x0010
#define NPP_CTRL_CIRCLE        0x0020
#define NPP_CTRL_CROSS         0x0040
#define NPP_CTRL_SQUARE        0x0080

/* button[NPP_BTN_LDD] bits - only meaningful for LDD pads. This is the one
 * that actually makes the XMB react to the PS button. */
#define NPP_CTRL_LDD_PS        0x0001

/* Device capability bits reported through cellPadGetInfo2(). Games that
 * "require an official DualShock 3" are almost always testing PS3_CONFORMITY. */
#define NPP_CAP_PS3_CONFORMITY 0x0001
#define NPP_CAP_PRESS_MODE     0x0002
#define NPP_CAP_SENSOR_MODE    0x0004
#define NPP_CAP_HP_ANALOG      0x0008
#define NPP_CAP_ACTUATOR       0x0010

#define NPP_SETTING_PRESS_ON   0x0002
#define NPP_SETTING_SENSOR_ON  0x0004

#define NPP_STATUS_CONNECTED       0x0001
#define NPP_STATUS_ASSIGN_CHANGES  0x0002

#define NPP_DEV_TYPE_STANDARD  0
#define NPP_DEV_TYPE_LDD       5

/* NPP_STICK_CENTER (npp_state.h) and NPP_SENSOR_CENTER (sixaxis.h), the
 * neutral values of a DS3 at rest, are in headers the kernel shim builds. */

/* ------------------------------------------------------------------------ */
/* Controller models                                                         */
/* ------------------------------------------------------------------------ */

#define NPP_VID_SONY           0x054c
#define NPP_PID_DS4_V1         0x05c4  /* CUH-ZCT1 */
#define NPP_PID_DS4_V2         0x09cc  /* CUH-ZCT2 */
#define NPP_PID_DS4_DONGLE     0x0ba0  /* DUALSHOCK 4 USB Wireless Adaptor */
#define NPP_PID_DUALSENSE      0x0ce6  /* CFI-ZCT1 */
#define NPP_PID_DUALSENSE_EDGE 0x0df2  /* CFI-ZCT1W */

typedef enum {
	NPP_MODEL_UNKNOWN = 0,
	NPP_MODEL_DS4,          /* DualShock 4, either revision + dongle */
	NPP_MODEL_DUALSENSE,    /* DualSense and DualSense Edge          */
} npp_model_t;

typedef enum {
	NPP_LINK_USB = 0,
	NPP_LINK_BT,            /* reports carry the BT framing + CRC32 */
} npp_link_t;

/* ------------------------------------------------------------------------ */
/* Normalised controller state                                               */
/*                                                                           */
/* npp_state_t, its NPP_B_* bits and npp_map_t are in npp_state.h, which */
/* has no SDK dependency so the kernel shim can build against it too. It is  */
/* included at the top of this file.                                         */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Output (rumble / lightbar / player LEDs)                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint8_t rumble_weak;    /* right / high frequency motor */
	uint8_t rumble_strong;  /* left  / low  frequency motor */
	uint8_t led_r, led_g, led_b;
	uint8_t player;         /* 1..4, 0 = unassigned         */
	uint8_t dirty;
} npp_output_t;

/* ------------------------------------------------------------------------ */
/* Configuration                                                             */
/* ------------------------------------------------------------------------ */

typedef enum {
	NPP_MODE_AUTO = 0,   /* USB when cabled, otherwise BT passthrough */
	NPP_MODE_USB,        /* USB only                                  */
	NPP_MODE_BT,         /* internal-Bluetooth passthrough only       */
} npp_mode_t;

typedef enum {
	NPP_PRESSURE_DIGITAL = 0, /* held -> 255                            */
	NPP_PRESSURE_RAMP,        /* held -> ramps up over PRESSURE_RAMP_MS */
} npp_pressure_t;

/* ------------------------------------------------------------------------ */
/* Analog stick calibration                                                  */
/*                                                                           */
/* One of these per axis, per controller. `min`/`max` are the extremes the    */
/* stick has actually been seen to reach and `ctr` its resting position; the  */
/* transform in sticks.c normalises against them before applying the DS3      */
/* saturation curve. Everything is learned at runtime, so a Hall effect       */
/* module that only swings 0x14..0xEB still ends up producing full travel.    */
/* ------------------------------------------------------------------------ */

#define NPP_CAL_BUCKETS 16   /* histogram resolution: 256 / 16 = 16 units */

typedef struct {
	uint8_t  min, max, ctr;
	uint8_t  rest_cand;      /* candidate resting value being confirmed   */
	uint16_t rest_hits;
	uint16_t centre_budget;  /* samples left in which ctr may still move  */
	uint8_t  hist[NPP_CAL_BUCKETS]; /* raw values seen this window       */
	uint16_t win_count;      /* samples into the current window           */
} npp_axis_cal_t;

/* Axis order: LX, LY, RX, RY. */
#define NPP_AXIS_LX 0
#define NPP_AXIS_LY 1
#define NPP_AXIS_RX 2
#define NPP_AXIS_RY 3
#define NPP_AXIS_NUM 4

typedef struct {
	npp_axis_cal_t axis[NPP_AXIS_NUM];
	int             dirty;
} npp_stickcal_t;

typedef enum {
	NPP_TOUCH_OFF = 0,
	NPP_TOUCH_BUTTONS,   /* left/right halves act as two buttons */
	NPP_TOUCH_RSTICK,    /* finger position drives right stick   */
} npp_touchmode_t;

/* Substitute for the PS button when the transport cannot deliver a real one
 * (Bluetooth passthrough). The combination is swallowed, so the games never
 * see the buttons it is built from. */
typedef enum {
	NPP_PSCOMBO_NONE = 0,
	NPP_PSCOMBO_SELECT_START,
	NPP_PSCOMBO_L3_R3,
} npp_pscombo_t;

/* npp_map_t, the symbolic targets a physical control can be remapped onto
 * (NPP_MAP_*), is in npp_state.h: the kernel shim honours two of the maps. */

typedef struct {
	/* Master switch. With this off the plugin loads, writes its boot trace
	 * and then does nothing at all - the baseline for telling "the module is
	 * harmless" apart from "something the module does breaks the console". */
	int              enable;
	/* Whether injected samples are made visible to games as well as the XMB.
	 * On by default; turning it off is the way to find out whether that is
	 * what stalls an app while it loads. */
	int              insert_into_game;
	int              allow_insert_combo; /* legacy service shortcut, opt-in */

	npp_mode_t      mode;
	npp_pressure_t  pressure;
	uint32_t         pressure_ramp_ms;
	npp_touchmode_t touch_mode;

	uint32_t poll_ms;          /* LDD injection period                    */
	/* Hard limit on virtual pads. One by default: a single controller is the
	 * normal case, and the cap is what stops a runaway from filling every
	 * port on the console. */
	uint32_t max_pads;

	/* Sticks. `saturation` is the headline control: the percentage of
	 * measured travel that counts as full deflection. A DualShock 3's ADC
	 * saturates at roughly 80% of its mechanical range, which is why its
	 * corners read far closer to 0x00/0xFF than a linear DS4/DualSense.
	 *   100 = raw, no correction
	 *    80 = DualShock 3 equivalent (default)
	 *    71 = diagonals reach the corners exactly */
	uint32_t stick_saturation; /* 50..100                                 */
	uint32_t stick_deadzone;   /* radial, percent of travel               */
	int      stick_calibrate;  /* learn each stick's real range at runtime */

	int      sixaxis;          /* master enable for motion forwarding     */
	int      accel_scale;      /* percent, 100 = physically correct       */
	int      gyro_scale;       /* percent                                 */
	int      invert_x, invert_y, invert_z, invert_g;
	int      swap_xy;

	int      rumble;
	int      lightbar;         /* colour the lightbar per player          */
	int      dim_lightbar;     /* keep it dim to save battery             */

	/* Capability bits advertised to games through cellPadGetInfo2(). The
	 * five documented bits are the default; PS3xPAD historically passed
	 * 0xFFFF, which is worth trying if a game still refuses the pad. */
	uint32_t capability_mask;
	/* How far the mask is shifted before going into syscall 574. Every
	 * published plugin uses 1; measured behaviour suggests the system
	 * grants only a subset, so this is exposed for experimentation. */
	uint32_t capability_shift;
	/* Device type reported for the virtual pad. 5 = LDD, which is what the
	 * syscall is normally given; a real DualShock 3 reports 0 (STANDARD),
	 * and a game that gates on the type rather than on the conformity bit
	 * will only accept 0. */
	uint32_t ldd_dev_type;

	npp_map_t map_touchpad;   /* touchpad click target                   */
	npp_map_t map_mute;       /* DualSense mute button target            */
	npp_map_t map_touch_left; /* touch zones when touch_mode = BUTTONS   */
	npp_map_t map_touch_right;
	npp_map_t map_fn_l, map_fn_r, map_paddle_l, map_paddle_r;

	/* Passthrough mode: which OEM port to read, -1 = auto-detect. */
	int      bt_source_port;
	/* Where the PS button lands inside the OEM pad report, if anywhere:
	 * an index into CellPadData.button[] plus a bit mask, or -1 for "not
	 * reported". Discovered with the diagnostic dump - docs/DIAGNOSTICS.md. */
	int      bt_ps_code;
	int      bt_ps_mask;
	npp_pscombo_t bt_ps_combo;

	/* In-game build only: answer cellPadGetInfo2 as a conformant DualShock 3
	 * so a game that gates on device type accepts whatever pad is present. */
	int      spoof_pad_info;
	/* Only for passthrough: raw USB ownership needs the full controller. */
	int      ps_only;

	int      diag;             /* write NPP_DIAG_FILE                    */
	int      notify;           /* show XMB notifications                  */
	/* ENABLE_SHIM (0/1): install the DS3-route kernel shim (kshim.c) when the
	 * plugin loads. Off by default. The shim patches live kernel code, and a build
	 * that gets a patch wrong can hang the console, so a deployed plugin never
	 * installs it on its own: nativepadps3.txt has to say ENABLE_SHIM = 1. That is also
	 * the way out of a hang - remove the line (or set it to 0) and reboot. */
	int      enable_shim;
	/* SHIM_INLINE_PID (0/1, VSH builds only): with the shim installed, also
	 * edit the kernel's inline product-ID compare (two instruction words) and not
	 * only its helper. Off by default; the console test decides whether the
	 * DualSense needs it. */
	int      shim_inline_pid;
	/* PROBE_EXEC (0/1, VSH builds only): run the debug-only executability
	 * pre-flight (design Amendment 2) instead of installing the shim this boot -
	 * poke a leaf stub into the fixed exec region, call it once and trace what it
	 * returned, proving the region runs before any hook is wired. Off by default;
	 * only the debug build acts on it. */
	int      probe_exec;
	/* SHIM_INPUT_HOOK (0/1, VSH builds only): gate whether the shim installs its
	 * per-poll Bluetooth input redirect (0x37CE38). On by default, so a normal
	 * install is unchanged; set to 0 to install the start-up and enable completion
	 * hooks WITHOUT the input hook, which isolates the input hook (the only one
	 * that runs on every poll and shares the Bluetooth transport vtable) when
	 * finding the DS3 interaction on hardware (design Amendment 3). */
	int      shim_input_hook;
	/* SHIM_EVENT_HOOK / SHIM_ENABLE_HOOK (0/1, VSH builds only): gate the start-up
	 * event (0x37CFF8) and enable (0x37CFB0) completion redirects, like
	 * SHIM_INPUT_HOOK. Both on by default. Setting all three hook gates to 0 leaves
	 * only the product-ID word edits installed, which isolates the completion hooks
	 * from the helper patch while finding the DS3 interaction on hardware. */
	int      shim_event_hook;
	int      shim_enable_hook;
	/* SHIM_STABLE_ID (0/1, VSH builds only): give each DualSense the identity of its
	 * own Bluetooth address, read by the shim from the kernel's connection table, so
	 * the same controller keeps the same identity across reconnects (stage 1b). On by
	 * default; 0 makes the shim use its per-boot counter identity instead, which is
	 * also what it falls back to whenever the address cannot be read safely. */
	int      shim_stable_id;
	/* SHIM_OUTPUT_HOOK (0/1, VSH builds only): install the hook on the kernel's
	 * Bluetooth output call so a DualSense gets the rumble, player LEDs and light
	 * bar the PS3 asks of a DualShock 3. On by default. SHIM_OUTPUT_MODE (0..2,
	 * clamped; default 2) picks the stage: 0 observe only (capture and count, pass
	 * the call through), 1 probe (fixed blue light bar and player LED 1, for
	 * tests), 2 full translation (rumble, player LEDs and light bar colour by
	 * controller number). Modes 1 and 2 need the input hook (SHIM_INPUT_HOOK, default
	 * 1): they let the DualSense switch to its 79-byte input report, which only the
	 * input redirect can take in safely. With SHIM_INPUT_HOOK = 0 the output hook
	 * only observes (the shim writes out_mode 0 and installs neither the send-length
	 * cap nor the receive limits), whatever SHIM_OUTPUT_MODE says. */
	int      shim_output_hook;
	int      shim_output_mode;
	/* SHIM_POWEROFF_HOOK (0/1, VSH builds only): install the hook that lets the
	 * console turn a DualSense off, the way it turns a DualShock 3 off. On by
	 * default. SHIM_POWEROFF_MODE (0..1, clamped; default 1) picks what it does:
	 * 0 observe only (record the request, pass the call through), 1 act (power the
	 * DualSense off). Debug VSH builds on DEX 4.92 only, like the rest of the shim. */
	int      shim_poweroff_hook;
	int      shim_poweroff_mode;
#ifdef NPP_LV2DUMP
	int      dump_lv2;         /* DUMP_LV2: debug VSH builds only, see lv2dump.c */
	uint32_t capture_off;      /* CAPTURE_LV2: LV2 offset to watch, 8-aligned  */
	uint32_t capture_len;      /* CAPTURE_LEN: bytes, multiple of 8, 0 = off   */
	uint32_t capture_ptr;      /* CAPTURE_PTR: LV2 offset of a pointer qword;
	                            * nonzero = watch CAPTURE_LV2 bytes into the
	                            * object it points to, 0 = off                 */
#endif
} npp_config_t;

extern npp_config_t g_cfg;

void npp_config_defaults(npp_config_t *c);
int  npp_config_load(npp_config_t *c, const char *path);

/* ------------------------------------------------------------------------ */
/* Per-controller runtime slot                                               */
/* ------------------------------------------------------------------------ */

#define NPP_MAX_PADS 4

typedef struct npp_pad {
	int           in_use;
	npp_model_t  model;
	npp_link_t   link;

	/* USB plumbing (unused in passthrough mode). */
	int32_t       dev_id;
	int32_t       c_pipe, i_pipe, o_pipe;
	int32_t       in_size, out_size;

	/* Passthrough plumbing. */
	int           src_port;

	/* USB callbacks copy completed transfers into reports under usb_mutex.
	 * The polling thread parses private copies in arrival order. */
	npp_state_t  state;
	int           buf_idx;

	/* libpad LDD handle and the port the system gave us. */
	int32_t       handle;
	int32_t       port;
	int           insert_on;   /* is the pad currently visible to games? */
	int           combo_held;   /* debounced state of the toggle combo   */
	int           combo_stable; /* samples the new state has held        */
	/* Set by the USB attach callback, acted on by the polling thread.
	 * Registering the virtual pad involves a 10 ms sleep and several file
	 * writes, and doing that inside a USB enumeration callback blocks the
	 * USB manager thread - which is exactly what apps wait on while they
	 * start up. The callback must set up the slot and return immediately. */
	volatile int  needs_register;
	int           read_fails;   /* consecutive source-read failures */
	int           read_blocked; /* reads failing while the port is present */
	int           inject_error;
	int           ps_sent;
	int           ps_seeded;
	npp_report_queue_t reports;
	int           usb_ps_suppressed;
	int           usb_report_seen;
	int           usb_error;
	int           usb_error_reported;

	npp_output_t   out;
	npp_stickcal_t cal;
	uint32_t      press_ticks[16];   /* ramp accumulators, per press code */
	uint32_t      out_seq;           /* DualSense Bluetooth sequence tag  */

	/* USB transfer buffers. These must outlive each transfer, so they live
	 * in the slot rather than on a stack. */
	uint8_t       buf[2][80];
	uint8_t       obuf[80];
} npp_pad_t;

extern npp_pad_t g_pads[NPP_MAX_PADS];

/* ------------------------------------------------------------------------ */
/* Module interfaces                                                         */
/* ------------------------------------------------------------------------ */

/* ds4.c / dualsense.c - HID report translation. The DualSense field decoding
 * itself is ds_body.c (dualsense_parse_bt_body, declared in npp_state.h). */
int  ds4_parse(const uint8_t *buf, int len, npp_link_t link, npp_state_t *st);
int  ds4_build_output(uint8_t *buf, int cap, npp_link_t link,
                      const npp_output_t *out);
int  dualsense_parse(const uint8_t *buf, int len, npp_link_t link,
                     npp_state_t *st);
int  dualsense_build_output(uint8_t *buf, int cap, npp_link_t link,
                            const npp_output_t *out);

/* sticks.c - calibration and the DualShock 3 stick response curve */
void sticks_cal_defaults(npp_stickcal_t *cal);
void sticks_load(void);
void sticks_save_if_dirty(int force);
void sticks_transform(npp_pad_t *pad, const npp_state_t *st,
                      uint8_t *lx, uint8_t *ly, uint8_t *rx, uint8_t *ry);

/* padldd.c - virtual controller */
int  ldd_register(npp_pad_t *pad);
int  ldd_set_insert_into_game(npp_pad_t *pad, int on);
void ldd_check_combo(npp_pad_t *pad);
void ldd_unregister(npp_pad_t *pad);
void ldd_inject(npp_pad_t *pad);
void ldd_translate(npp_pad_t *pad, const npp_state_t *st, uint16_t *button);

/* usb.c - cellUsbd device driver */
int  usb_init(void);
void usb_poll(void);
void usb_report_probes(void);
void usb_shutdown(void);
void usb_send_output(npp_pad_t *pad);

/* passthru.c - internal Bluetooth passthrough */
int  passthru_scan(void);   /* rescan tick: virtual pads registered by this call */
void passthru_poll(void);
void passthru_release(void);
/* Before the VSH detours are installed: cross-check lock and state. -1 if the
 * lock could not be created (the cross-check then never sees VSH samples, so
 * it never passes). passthru_shutdown() after the detours are removed. */
int  passthru_init(void);
void passthru_shutdown(void);
/* From the VSH pad-read detours, before any masking. Never blocks: the sample
 * is dropped if the plugin thread holds the lock. */
void passthru_observe_vsh(uint32_t port_no, const CellPadData *data);
/* Plugin thread only (they take the lock): btcheck_state() as an int,
 * btcheck_vsh_samples_seen(), and raw samples fed since passthru_init(). */
int  passthru_btcheck_state(void);
int  passthru_btcheck_vsh_samples(void);
int  passthru_raw_samples(void);
#ifdef NPP_DEBUG
void passthru_trace_vsh_reads(void);   /* boot trace: what VSH's pad reads look like */
#endif
void npp_process_bt_input(uint32_t port_no, CellPadData *data);
void npp_trigger_bt_ps(int pressed);

/* gamehook.c - in-game stick correction (NPP_GAME build) */
int  gamehook_install(void);
void gamehook_remove(void);

/* diag.c */
void diag_dump_ports(void);
void diag_watch(void);
void diag_state(const npp_pad_t *pad);
void diag_sticks(uint8_t in_x, uint8_t in_y, uint8_t out_x, uint8_t out_y,
                 const npp_axis_cal_t *cx, const npp_axis_cal_t *cy);
void diag_msg(const char *msg);
void diag_observe(int port, const void *data);
void diag_flush(void);

/* util.c */
void  npp_notify(const char *msg);
void  npp_boot_mark(const char *stage);
void  npp_boot_mark_hex(const char *stage, uint32_t value);
uint32_t npp_crc32(uint32_t crc, const uint8_t *data, int len);
int   npp_read_file(const char *path, char *out, int cap);
int   npp_write_file(const char *path, const void *data, int len);  /* create/truncate; 0 ok, -1 fail */
#ifndef NPP_GAME   /* VSH-only: would add libfs imports to the game build */
int   npp_delete_file(const char *path);                            /* 0 ok, -1 fail */
int   npp_file_exists(const char *path);                            /* 1 yes, 0 no */
int   npp_append_file(const char *path, const void *data, int len); /* create or append; 0 ok, -1 fail */
#endif

#ifdef NPP_LV2DUMP
/* lv2dump.c: two dumps of the LV2 window once a Bluetooth pad is connected
 * (A untouched, B with Cross held and the left stick left). Zero-initialise
 * the state, then poll every tick; it stops by itself when done. */
typedef struct {
	int      phase;
	int      said_wait;
	int      checked;          /* existing dumps looked at once */
	int      opt_down;         /* button mode: Options seen pressed */
	int      held;             /* button mode: Cross held since due_ms - HOLD */
	uint32_t due_ms;
} lv2dump_t;
void  lv2dump_poll(lv2dump_t *st, uint32_t now_ms);
/* passthru.c: any generic (Bluetooth) pad connected, bound or not. */
int   passthru_bt_pad_present(void);
/* passthru.c: the bound pad's d1/d2 (NPP_CTRL_*) from this tick's raw
 * report; 0 when there is none (no record, not bound, not connected). */
int   passthru_source_buttons(uint16_t *d1, uint16_t *d2);

/* lv2capture.c: watch CAPTURE_LEN bytes at LV2 offset CAPTURE_LV2 every
 * tick and write each change to the boot trace, bounded in total. Zero-
 * initialise the state, then poll every tick. */
#define NPP_CAPTURE_MAX_LEN  128
#define NPP_CAPTURE_MAX_LOGS 400
typedef struct {
	int      have;             /* snap holds the last logged bytes */
	int      stopped;          /* read refused or log limit reached */
	uint32_t logs;             /* changes written so far */
	uint64_t ptr;              /* CAPTURE_PTR mode: last pointer value seen */
	int      ptr_bytes;        /* CAPTURE_PTR mode: snap holds its bytes */
	uint8_t  snap[NPP_CAPTURE_MAX_LEN];
} lv2capture_t;
void  lv2capture_poll(lv2capture_t *st, uint32_t now_ms);
#endif

/* Debug-build-only boot trace. Compiles to nothing (and the tag strings are
 * not emitted) unless built with -DNPP_DEBUG. */
#ifdef NPP_DEBUG
#define NPP_TRACE(tag)         npp_boot_mark(tag)
#define NPP_TRACE_HEX(tag, v)  npp_boot_mark_hex((tag), (uint32_t)(v))
#else
#define NPP_TRACE(tag)         ((void)0)
#define NPP_TRACE_HEX(tag, v)  ((void)0)
#endif
npp_map_t npp_parse_map(const char *name);
void  npp_apply_map(npp_map_t m, uint16_t *button);

/* Number of controllers currently producing input. */
int   npp_active_pads(void);
int   npp_usb_pads_active(void);
int   npp_bt_pads_active(void);

#endif /* NPP_H */
