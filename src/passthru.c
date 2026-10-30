/*
 * DS45PAD - internal Bluetooth passthrough.
 *
 * A DualShock 4 or DualSense paired to the console's own Bluetooth radio shows
 * up as a generic HID gamepad: sticks and face buttons work, but the PS button
 * does nothing, there is no pressure sensitivity, no motion, no rumble, and
 * games that check for an official controller reject it.
 *
 * This path receives CellPadData, NOT raw Bluetooth or USB HID reports.
 * It cannot recover a PS bit absent from that data. A configured, measured
 * code/mask can forward a bit the firmware does expose. Legacy substitution
 * combos are opt-in and are not evidence of reading the physical PS button.
 * Raw USB is a separate path and requires successful device acquisition.
 *
 * The raw Bluetooth report is read from the kernel object a slot points to;
 * where that slot is comes from kernel_active() (kernel.c), never from an
 * address in this file. A source pad is bound as soon as one shows up, but
 * its virtual pad is only registered once the passive cross-check
 * (btcheck.c) has PASSED. If the check FAILS the record is rejected, every
 * Bluetooth pad is released and nothing more is adopted: the console's own
 * driver keeps the controller.
 */

#include <string.h>
#include <sys/process.h>
#include <sys/synchronization.h>
#include <sys/sys_time.h>
#include <cell/pad.h>

#include "ds45pad.h"
#include "lv2.h"
#include "kernel.h"
#include "btcheck.h"

/* ------------------------------------------------------------------------ */
/* Cross-check state and its lock                                            */
/*                                                                           */
/* btcheck is a single instance without locking of its own. Its VSH side is   */
/* fed from the cellPadGetData detours, on VSH's thread; its raw side from    */
/* passthru_poll on the plugin thread. bc_mutex serialises every btcheck_*    */
/* call. The VSH side only ever trylocks and drops the sample when the lock   */
/* is taken (a dropped VSH sample is neutral), so VSH is never blocked.       */
/* ------------------------------------------------------------------------ */

static sys_mutex_t bc_mutex;
static int         bc_ready;

/* Port the cross-check watches; -1 = none. Written by the plugin thread,
 * read without the lock by the VSH detours only as a cheap pre-filter. */
static volatile int s_src_port = -1;
static uint64_t s_bt_work;         /* object holding the report; 0 = not usable */
static int      s_raw_samples;     /* raw samples fed to the cross-check */

/* Without the mutex (creation failed) the VSH side never touches btcheck, so
 * the plugin thread is its only user and needs no lock. */
static void bc_lock(void)   { if (bc_ready) sys_mutex_lock(bc_mutex, 0); }
static void bc_unlock(void) { if (bc_ready) sys_mutex_unlock(bc_mutex); }

int passthru_init(void)
{
	sys_mutex_attribute_t attr;

	s_src_port    = -1;
	s_bt_work     = 0;
	s_raw_samples = 0;

	bc_ready = 0;
	sys_mutex_attribute_initialize(attr);
	if (sys_mutex_create(&bc_mutex, &attr) == 0)   /* CELL_OK */
		bc_ready = 1;
	else
		ds45_boot_mark("btcheck_mutex_FAILED");

	/* Watch nothing until a source pad is bound. */
	btcheck_reset(-1);
	return bc_ready ? 0 : -1;
}

void passthru_shutdown(void)
{
	if (bc_ready) {
		bc_ready = 0;
		sys_mutex_destroy(bc_mutex);
	}
}

#ifdef DS45_DEBUG
/* What VSH's own pad reads look like, for the boot trace. Written from VSH's
 * threads without a lock: plain counters, approximate by design. */
static volatile uint32_t s_dbg_calls;       /* every hooked read */
static volatile uint32_t s_dbg_ports;       /* bit n: port n was read */
static volatile uint32_t s_dbg_src_calls;   /* reads of the source port */
static volatile uint32_t s_dbg_src_changes; /* ... whose data differed */
static volatile uint32_t s_dbg_src_len;     /* last len on the source port */
static volatile uint32_t s_dbg_src_word;    /* last d1<<24|d2<<16|lx<<8|ly */
static volatile uint32_t s_dbg_busy;        /* trylock failures */

static void dbg_observe(uint32_t port_no, const CellPadData *data)
{
	uint32_t w = 0;

	s_dbg_calls++;
	if (port_no < 32)
		s_dbg_ports |= 1u << port_no;
	if ((int)port_no != s_src_port)
		return;
	s_dbg_src_calls++;
	s_dbg_src_len = (uint32_t)data->len;
	if (data->len > DS45_BTN_DIGITAL2)
		w |= (uint32_t)(data->button[DS45_BTN_DIGITAL1] & 0xFF) << 24 |
		     (uint32_t)(data->button[DS45_BTN_DIGITAL2] & 0xFF) << 16;
	if (data->len > DS45_BTN_ANALOG_LEFT_Y)
		w |= (uint32_t)(data->button[DS45_BTN_ANALOG_LEFT_X] & 0xFF) << 8 |
		     (uint32_t)(data->button[DS45_BTN_ANALOG_LEFT_Y] & 0xFF);
	if (w != s_dbg_src_word)
		s_dbg_src_changes++;
	s_dbg_src_word = w;
}

void passthru_trace_vsh_reads(void)
{
	DS45_TRACE_HEX("vsh_reads", s_dbg_calls);
	DS45_TRACE_HEX("vsh_ports", s_dbg_ports);
	DS45_TRACE_HEX("vsh_src_reads", s_dbg_src_calls);
	DS45_TRACE_HEX("vsh_src_changes", s_dbg_src_changes);
	DS45_TRACE_HEX("vsh_src_len", s_dbg_src_len);
	DS45_TRACE_HEX("vsh_src_d1d2lxly", s_dbg_src_word);
	DS45_TRACE_HEX("vsh_lock_busy", s_dbg_busy);
	DS45_TRACE_HEX("raw_samples", s_raw_samples);
}
#endif

void passthru_observe_vsh(uint32_t port_no, const CellPadData *data)
{
#ifdef DS45_DEBUG
	if (data)
		dbg_observe(port_no, data);
#endif
	/* VSH reads every port at frame rate; only the watched one is worth a
	 * lock (btcheck ignores the others anyway). */
	if (!bc_ready || !data || (int)port_no != s_src_port)
		return;
	if (sys_mutex_trylock(bc_mutex) != 0) { /* CELL_OK */
#ifdef DS45_DEBUG
		s_dbg_busy++;
#endif
		return;                         /* plugin thread busy: drop it */
	}
	btcheck_observe_vsh(port_no, data);
	sys_mutex_unlock(bc_mutex);
}

int passthru_btcheck_state(void)
{
	int s;

	bc_lock();
	s = (int)btcheck_state();
	bc_unlock();
	return s;
}

int passthru_btcheck_vsh_samples(void)
{
	int n;

	bc_lock();
	n = btcheck_vsh_samples_seen();
	bc_unlock();
	return n;
}

int passthru_raw_samples(void)
{
	return s_raw_samples;
}

/* ------------------------------------------------------------------------ */
/* Raw report                                                                */
/* ------------------------------------------------------------------------ */

/* The slot's pointer must lie inside the LV2 window, and the 16 report bytes
 * at pointer + report_off must be two aligned, in-window qwords. */
static int bt_work_usable(uint64_t v, uint32_t report_off)
{
	uint64_t r;

	if (v < DS45_LV2_BASE || v >= DS45_LV2_BOUND)
		return 0;
	r = v + report_off;            /* report_off <= DS45_REPORT_OFF_MAX */
	return (r & 7) == 0 && r <= DS45_LV2_BOUND - 16;
}

/* The slot is NULL until the Bluetooth stack is up, and may be repointed if
 * it restarts: re-read on every rescan tick. A bad value is never kept or
 * cached; the next tick simply tries again. */
static void refresh_bt_work(const ds45_kernel_t *k)
{
	uint64_t v;

	if (ds45_lv2_read(k->slot_addr, &v) != 0 || !bt_work_usable(v, k->report_off))
		v = 0;
	if (v != s_bt_work)
		DS45_TRACE_HEX("bt_work_lo", v);
	s_bt_work = v;
}

/* w0, w1, then w0 again: if w0 changed, the kernel was rewriting the report
 * while we read it and the sample is dropped. 0 and *out set on a sample
 * that decodes (header A1 01, hat <= 8). */
static int read_raw(ds45_btraw_t *out)
{
	const ds45_kernel_t *k = kernel_active();
	uint64_t a, w0, w1, w0_again;

	if (!k || !s_bt_work)
		return -1;
	a = s_bt_work + k->report_off;
	if (ds45_lv2_read(a, &w0) != 0 || ds45_lv2_read(a + 8, &w1) != 0 ||
	    ds45_lv2_read(a, &w0_again) != 0)
		return -1;
	if (w0 != w0_again)
		return -1;
	return bt_report_decode(w0, w1, out);
}

/* Ports occupied by pads we created ourselves.
 *
 * Checking device_type alone is not enough: a freshly registered virtual pad
 * does not necessarily report type 5 straight away, and adopting one of our
 * own pads as if it were a physical controller starts a cascade that consumes
 * every slot. The authoritative answer is our own bookkeeping. */
static int port_is_ours(int port)
{
	int i;

	for (i = 0; i < DS45_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].handle >= 0 &&
		    g_pads[i].port == port)
			return 1;

	return 0;
}

/* A real DualShock 3 advertises PS3 conformity; a generic Bluetooth HID pad
 * never does. That is the cleanest way to tell the two apart. */
static int is_generic_pad(const CellPadInfo2 *info, int port)
{
	if (!(info->port_status[port] & DS45_STATUS_CONNECTED))
		return 0;
	if (info->device_type[port] == DS45_DEV_TYPE_LDD)
		return 0;
	if (info->device_capability[port] & DS45_CAP_PS3_CONFORMITY)
		return 0;
	if (port_is_ours(port))
		return 0;

	return 1;
}

#ifdef DS45_LV2DUMP
/* The bound pad's buttons from this tick's raw report, for the dumper's
 * button triggers. Written and read on the plugin thread only. */
static int      s_dump_btn_valid;
static uint16_t s_dump_d1, s_dump_d2;

int passthru_source_buttons(uint16_t *d1, uint16_t *d2)
{
	if (!s_dump_btn_valid)
		return 0;
	*d1 = s_dump_d1;
	*d2 = s_dump_d2;
	return 1;
}

/* The debug LV2 dumper waits for this. Not limited to the bound pad: a
 * firmware without a kernel record never binds one, and that is exactly the
 * firmware a dump is for. */
int passthru_bt_pad_present(void)
{
	CellPadInfo2 info;
	int port;

	if (cellPadGetInfo2(&info) != CELL_PAD_OK)
		return 0;
	for (port = 0; port < DS45_MAX_PORT_NUM; port++)
		if (is_generic_pad(&info, port))
			return 1;

	return 0;
}
#endif

static int already_bound(int port)
{
	int i;

	for (i = 0; i < DS45_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == DS45_LINK_BT &&
		    g_pads[i].src_port == port)
			return 1;

	return 0;
}

static ds45_pad_t *alloc_slot(void)
{
	int i;

	for (i = 0; i < DS45_MAX_PADS; i++)
		if (!g_pads[i].in_use)
			return &g_pads[i];

	return NULL;
}

static ds45_pad_t *bound_pad(void)
{
	int i;

	for (i = 0; i < DS45_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == DS45_LINK_BT)
			return &g_pads[i];

	return NULL;
}

/* Bind the source pad: claim a slot for it and point the cross-check at its
 * port, without registering a virtual pad yet. One at a time: there is one
 * raw report and one cross-check. */
static ds45_pad_t *bind_source(void)
{
	CellPadInfo2 info;
	int port;

	if (cellPadGetInfo2(&info) != CELL_PAD_OK)
		return NULL;

	/* Walk downwards: users are told to park the Bluetooth pad on the last
	 * port so the virtual one can take port 1, so the highest matching port
	 * is almost always the right source. */
	for (port = DS45_MAX_PORT_NUM - 1; port >= 0; port--) {
		ds45_pad_t *pad;

		if (g_cfg.bt_source_port >= 0 && port != g_cfg.bt_source_port)
			continue;
		if (!is_generic_pad(&info, port) || already_bound(port))
			continue;

		pad = alloc_slot();
		if (!pad)
			return NULL;

		/* Same as the USB path: keep the calibration across the clear. */
		{
			ds45_stickcal_t keep = pad->cal;

			memset(pad, 0, sizeof(*pad));
			pad->cal = keep;
		}

		pad->in_use   = 1;
		pad->link     = DS45_LINK_BT;
		pad->model    = DS45_MODEL_UNKNOWN;
		pad->src_port = port;
		pad->dev_id   = -1;
		pad->handle   = -1;
		pad->port     = -1;
		pad->c_pipe   = pad->i_pipe = pad->o_pipe = -1;

		/* Only ask for the extended report if the port actually claims to
		 * support it. The generic pad reports capability 0x00 - neither
		 * pressure nor sensors - and forcing both on it was leaving the port
		 * unreadable, which tore the virtual pad down and rebuilt it once a
		 * second. */
		{
			uint32_t want = 0;

			if (info.device_capability[port] & DS45_CAP_PRESS_MODE)
				want |= DS45_SETTING_PRESS_ON;
			if (info.device_capability[port] & DS45_CAP_SENSOR_MODE)
				want |= DS45_SETTING_SENSOR_ON;

			if (want)
				cellPadSetPortSetting((uint32_t)port, want);
		}

		/* A pass survives the same pad coming back on the same port;
		 * anything else starts the cross-check over. */
		bc_lock();
		if (!(btcheck_state() == BTCHECK_PASSED && port == s_src_port))
			btcheck_reset(port);
		bc_unlock();
		s_src_port = port;
		ds45_boot_mark_hex("passthru_bound_port", (uint32_t)port);
		return pad;
	}

	return NULL;
}

/* Rescan tick. Returns the number of virtual pads registered by this call.
 * Nothing at all happens without a resolved kernel record. */
int passthru_scan(void)
{
	const ds45_kernel_t *k = kernel_active();
	ds45_pad_t *pad;

	if (!k)
		return 0;

	refresh_bt_work(k);

	if (passthru_btcheck_state() == BTCHECK_FAILED) {
		/* Not a live report: drop the record (and its cache entry), let go
		 * of every Bluetooth pad and stop adopting new ones - with no
		 * active record, every later call returns above. */
		ds45_boot_mark("btcheck_FAILED_stepping_aside");
		kernel_reject_active();
		passthru_release();
		s_bt_work = 0;
		return 0;
	}

	pad = bound_pad();
	if (!pad)
		pad = bind_source();
	if (!pad || pad->handle >= 0)
		return 0;

	/* Bound, but no virtual pad before the cross-check has passed. */
	if (passthru_btcheck_state() != BTCHECK_PASSED)
		return 0;
	if (ldd_register(pad) < 0)
		return 0;                    /* stays bound; retried next tick */

	if (g_cfg.notify)
		ds45_notify("DS45Pad Bluetooth Attivo / Active");

	return 1;
}



/* Is the source port still listed as connected, regardless of whether reading
 * it happens to be working right now? */
static int source_still_connected(int port)
{
	CellPadInfo2 info;

	if (port < 0 || port >= DS45_MAX_PORT_NUM)
		return 0;
	if (cellPadGetInfo2(&info) != CELL_PAD_OK)
		return 1;   /* cannot tell - assume it is there and keep the pad */

	return (info.port_status[port] & DS45_STATUS_CONNECTED) ? 1 : 0;
}

void ds45_trigger_bt_ps(int pressed)
{
	static int s_last_ps = 0;
	int i;

	if (pressed == s_last_ps)
		return;
	s_last_ps = pressed;

	for (i = 0; i < DS45_MAX_PADS; i++) {
		ds45_pad_t *pad = &g_pads[i];
		CellPadData ldd_data;
		int r, s;

		if (!pad->in_use || pad->link != DS45_LINK_BT || pad->handle < 0)
			continue;

		memset(&ldd_data, 0, sizeof(ldd_data));
		ldd_data.len = DS45_PAD_LEN;
		if (pressed) {
			ldd_data.button[DS45_BTN_LDD]      = DS45_CTRL_LDD_PS;
			ldd_data.button[DS45_BTN_DIGITAL1] = DS45_CTRL_PS;
		}
		ldd_data.button[DS45_BTN_ANALOG_LEFT_X]  = DS45_STICK_CENTER;
		ldd_data.button[DS45_BTN_ANALOG_LEFT_Y]  = DS45_STICK_CENTER;
		ldd_data.button[DS45_BTN_ANALOG_RIGHT_X] = DS45_STICK_CENTER;
		ldd_data.button[DS45_BTN_ANALOG_RIGHT_Y] = DS45_STICK_CENTER;
		for (s = DS45_BTN_SENSOR_X; s <= DS45_BTN_SENSOR_G; s++)
			ldd_data.button[s] = DS45_SENSOR_CENTER;

		r = cellPadLddDataInsert(pad->handle, &ldd_data);
		if (r == CELL_PAD_OK) {
			ds45_boot_mark(pressed ? "BT_PS_LDD_PRESS_OK" : "BT_PS_LDD_RELEASE_OK");
		} else {
			ds45_boot_mark_hex("BT_PS_LDD_FAIL", (uint32_t)r);
		}
	}
}

#define SYSCALL8_OPCODE_PS3MAPI          0x7777
#define PS3MAPI_OPCODE_GET_ALL_PROC_PID  0x0021

static int check_is_game_running(void)
{
	uint32_t pids[16];
	int i, count = 0;
	int32_t ret;

	memset(pids, 0, sizeof(pids));
	system_call_3(8, SYSCALL8_OPCODE_PS3MAPI, PS3MAPI_OPCODE_GET_ALL_PROC_PID, (uint64_t)(uintptr_t)pids);
	ret = (int32_t)p1;

	if (ret == 0) {
		for (i = 0; i < 16; i++) {
			if (pids[i] != 0)
				count++;
		}
		return (count > 1) ? 1 : 0;
	}
	return -1; /* error / transient: keep current state */
}

static inline uint8_t deadzone8(uint8_t val)
{
	if (val >= 123 && val <= 133)
		return 128;
	return val;
}

/* Forward one decoded raw report (bt_report_decode in btcheck.c owns the
 * byte layout and the button mapping) to every registered Bluetooth pad. */
static void ds45_inject_bt_pad(const ds45_btraw_t *r)
{
	static int s_last_ps = 0;
	static int s_ps_grace = 0;
	static int s_neutral_sent = 0;
	static int s_is_game_cached = 0;
	static int s_game_check_tick = 0;
	static int s_game_streak = 0;
	static int s_vsh_streak = 0;
	static int s_logged_state = -1;
	static int s_vsh_menu_open = 0;
	static int s_ps_held_ticks = 0;
	static int s_menu_opened_by_this_hold = 0;
	static int s_menu_timer = 0;
	static int s_last_circle = 0;
	CellPadInfo2 pinfo;
	int intercepted = 0;
	int in_game = 0;
	int circle_pressed = 0;
	int i;

	const uint8_t lx = r->lx, ly = r->ly, rx = r->rx, ry = r->ry;
	const uint8_t l2_analog = r->l2, r2_analog = r->r2;
	const int ps_phys = r->ps ? 1 : 0;
	/* d1 already carries DS45_CTRL_PS when the PS button is down */
	const uint16_t d1 = r->d1;
	const uint16_t d2 = r->d2;
	const uint16_t ldd = ps_phys ? DS45_CTRL_LDD_PS : 0;

	/* Check if the system is intercepting pad inputs (in-game XMB active over a running title) */
	if (cellPadGetInfo2(&pinfo) == CELL_PAD_OK) {
		intercepted = (pinfo.system_info & 0x01) ? 1 : 0; /* bit 0 = CELL_PAD_INFO_INTERCEPTED */
	}

	/* Periodically check if a game/app process is running (every ~100ms) */
	if (++s_game_check_tick >= 10) {
		s_game_check_tick = 0;
		int g = check_is_game_running();
		if (g >= 0) {
			if (g == 1) {
				s_game_streak++;
				s_vsh_streak = 0;
				if (s_game_streak >= 1)
					s_is_game_cached = 1;
			} else {
				s_vsh_streak++;
				s_game_streak = 0;
				/* Require 5 consecutive VSH detections (~500ms) to exit game mode */
				if (s_vsh_streak >= 5)
					s_is_game_cached = 0;
			}
		}
	}

	in_game = s_is_game_cached || intercepted;

	if (in_game != s_logged_state) {
		s_logged_state = in_game;
		ds45_boot_mark(in_game ? "STATE_IN_GAME" : "STATE_IN_VSH");
		if (in_game) {
			s_vsh_menu_open = 0;
			for (i = 0; i < DS45_MAX_PADS; i++) {
				if (g_pads[i].in_use && g_pads[i].handle >= 0)
					ldd_set_insert_into_game(&g_pads[i], 1);
			}
		}
	}

	/* Physical PS button */
	if (ps_phys) {
		s_ps_held_ticks++;
		if (!in_game && s_ps_held_ticks >= 25 && !s_vsh_menu_open) {
			s_vsh_menu_open = 1;
			s_menu_opened_by_this_hold = 1;
			s_menu_timer = 3000; /* 30 seconds */
			ds45_boot_mark("VSH_MENU_OPEN");
		}
	} else {
		s_ps_held_ticks = 0;
	}

	if (ps_phys != s_last_ps) {
		s_last_ps = ps_phys;
		ds45_boot_mark(ps_phys ? "BT_PS_LDD_PRESS_OK" : "BT_PS_LDD_RELEASE_OK");

		if (!ps_phys) {
			if (s_menu_opened_by_this_hold) {
				/* User finished holding PS button to open menu. Keep menu open! */
				s_menu_opened_by_this_hold = 0;
			} else if (!in_game && s_vsh_menu_open) {
				/* PS button pressed and released again while menu was open: close menu */
				s_vsh_menu_open = 0;
				s_neutral_sent = 0;
				ds45_boot_mark("VSH_MENU_CLOSE_PS");
			}
		}
	}

	circle_pressed = (d2 & DS45_CTRL_CIRCLE) ? 1 : 0;

	/* Handle VSH Menu cancel/confirm/timeout */
	if (!in_game && s_vsh_menu_open) {
		if (s_menu_timer > 0)
			s_menu_timer--;

		if (s_last_circle && !circle_pressed) {
			/* Circle released: user canceled menu */
			s_vsh_menu_open = 0;
			s_neutral_sent = 0;
			ds45_boot_mark("VSH_MENU_CLOSE_CIRCLE");
		} else if (d2 & DS45_CTRL_CROSS) {
			/* Cross pressed: user confirmed an action, keep open 20s for Yes/No dialog */
			s_menu_timer = 2000;
		} else if (s_menu_timer == 0) {
			/* Menu timed out */
			s_vsh_menu_open = 0;
			s_neutral_sent = 0;
			ds45_boot_mark("VSH_MENU_TIMEOUT");
		}
	}
	s_last_circle = circle_pressed;

	for (i = 0; i < DS45_MAX_PADS; i++) {
		ds45_pad_t *pad = &g_pads[i];
		CellPadData ldd_data;
		int s;
		int should_gate = 0;

		if (!pad->in_use || pad->link != DS45_LINK_BT || pad->handle < 0)
			continue;

		/* In VSH (dashboard, no game running):
		 * The physical controller on pad->src_port is ALREADY communicating directly with VSH.
		 * If we inject duplicate sticks and buttons into VSH via the virtual pad, VSH
		 * receives every input TWICE (double input).
		 * Therefore, in VSH, the virtual pad MUST be gated (silent):
		 * It only injects the PS button when ps_phys is pressed (or during release).
		 * EXCEPTION: When the PS menu is opened in VSH (s_vsh_menu_open == 1),
		 * the PS menu modal dialog ONLY accepts confirmation (X) and cancel (O) from Player 1 (Port 0).
		 * So when s_vsh_menu_open is 1, un-gate so Player 1 can confirm or cancel!
		 *
		 * In a game (in_game == 1):
		 * Games ignore the physical pad on Port 6 (Controller 7) and ONLY listen to Player 1 on Port 0.
		 * Therefore, in a game, if pad->port == 0, we MUST inject full controller data!
		 * If pad->port > 0 in a game, gate it unless in-game XMB is intercepted. */
		if (!in_game) {
			if (!s_vsh_menu_open)
				should_gate = 1;
		} else if (pad->port > 0 && !intercepted) {
			should_gate = 1;
		}

		if (should_gate && !ps_phys && s_ps_grace == 0) {
			if (s_neutral_sent)
				continue;
			s_neutral_sent = 1;

			memset(&ldd_data, 0, sizeof(ldd_data));
			ldd_data.len = DS45_PAD_LEN;
			ldd_data.button[DS45_BTN_ANALOG_LEFT_X]  = DS45_STICK_CENTER;
			ldd_data.button[DS45_BTN_ANALOG_LEFT_Y]  = DS45_STICK_CENTER;
			ldd_data.button[DS45_BTN_ANALOG_RIGHT_X] = DS45_STICK_CENTER;
			ldd_data.button[DS45_BTN_ANALOG_RIGHT_Y] = DS45_STICK_CENTER;
			for (s = DS45_BTN_SENSOR_X; s <= DS45_BTN_SENSOR_G; s++)
				ldd_data.button[s] = DS45_SENSOR_CENTER;
			cellPadLddDataInsert(pad->handle, &ldd_data);
			continue;
		}

		s_neutral_sent = 0;

		memset(&ldd_data, 0, sizeof(ldd_data));
		ldd_data.len = DS45_PAD_LEN;

		ldd_data.button[DS45_BTN_LDD]            = ldd;
		ldd_data.button[DS45_BTN_DIGITAL1]       = should_gate ? (ps_phys ? DS45_CTRL_PS : 0) : d1;
		ldd_data.button[DS45_BTN_DIGITAL2]       = should_gate ? 0 : d2;
		ldd_data.button[DS45_BTN_ANALOG_LEFT_X]  = should_gate ? DS45_STICK_CENTER : deadzone8(lx);
		ldd_data.button[DS45_BTN_ANALOG_LEFT_Y]  = should_gate ? DS45_STICK_CENTER : deadzone8(ly);
		ldd_data.button[DS45_BTN_ANALOG_RIGHT_X] = should_gate ? DS45_STICK_CENTER : deadzone8(rx);
		ldd_data.button[DS45_BTN_ANALOG_RIGHT_Y] = should_gate ? DS45_STICK_CENTER : deadzone8(ry);

		/* Digital pressure values */
		if (!should_gate) {
			ldd_data.button[DS45_BTN_PRESS_RIGHT]    = (d1 & DS45_CTRL_RIGHT) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_LEFT]     = (d1 & DS45_CTRL_LEFT) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_UP]       = (d1 & DS45_CTRL_UP) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_DOWN]     = (d1 & DS45_CTRL_DOWN) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_TRIANGLE] = (d2 & DS45_CTRL_TRIANGLE) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_CIRCLE]   = (d2 & DS45_CTRL_CIRCLE) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_CROSS]    = (d2 & DS45_CTRL_CROSS) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_SQUARE]   = (d2 & DS45_CTRL_SQUARE) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_L1]       = (d2 & DS45_CTRL_L1) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_R1]       = (d2 & DS45_CTRL_R1) ? 255 : 0;
			ldd_data.button[DS45_BTN_PRESS_L2]       = l2_analog;
			ldd_data.button[DS45_BTN_PRESS_R2]       = r2_analog;
		}

		for (s = DS45_BTN_SENSOR_X; s <= DS45_BTN_SENSOR_G; s++)
			ldd_data.button[s] = DS45_SENSOR_CENTER;

		cellPadLddDataInsert(pad->handle, &ldd_data);
	}
}

void ds45_process_bt_input(uint32_t port_no, CellPadData *data)
{
	static int s_mask_ss_frames = 0;
	static int s_mask_lr_frames = 0;
	static uint16_t s_last_d1 = 0;
	uint16_t d1;
	int combo_select_start = 0;
	int combo_l3_r3 = 0;
	int ps_pressed = 0;

	if (!data || data->len <= DS45_BTN_DIGITAL1)
		return;

	d1 = data->button[DS45_BTN_DIGITAL1];

	if (d1 != s_last_d1) {
		s_last_d1 = d1;
		ds45_boot_mark_hex("BT_HOOK_D1", (uint32_t)d1);
	}

	/* Check physical PS button bit in case exposed */
	if ((d1 & DS45_CTRL_PS) || (data->button[DS45_BTN_LDD] & DS45_CTRL_LDD_PS))
		ps_pressed = 1;

	/* Check configured custom code/mask */
	if (g_cfg.bt_ps_code >= 0 && g_cfg.bt_ps_code < data->len &&
	    g_cfg.bt_ps_code < (int)(sizeof(data->button) / sizeof(data->button[0])) &&
	    (data->button[g_cfg.bt_ps_code] & g_cfg.bt_ps_mask))
		ps_pressed = 1;

	/* Combo 1: CREATE + OPTIONS (SELECT + START) */
	if ((d1 & (DS45_CTRL_SELECT | DS45_CTRL_START)) == (DS45_CTRL_SELECT | DS45_CTRL_START)) {
		combo_select_start = 1;
		ps_pressed = 1;
		s_mask_ss_frames = 12; /* keep masking for ~200ms on release */
	}

	/* Combo 2: L3 + R3 */
	if ((d1 & (DS45_CTRL_L3 | DS45_CTRL_R3)) == (DS45_CTRL_L3 | DS45_CTRL_R3)) {
		combo_l3_r3 = 1;
		ps_pressed = 1;
		s_mask_lr_frames = 12;
	}

	/* Swallow combo buttons while pressed and during release cooldown so they don't leak into game/XMB */
	if (combo_select_start || s_mask_ss_frames > 0) {
		data->button[DS45_BTN_DIGITAL1] &= ~(DS45_CTRL_SELECT | DS45_CTRL_START);
		if (!combo_select_start && s_mask_ss_frames > 0)
			s_mask_ss_frames--;
	}

	if (combo_l3_r3 || s_mask_lr_frames > 0) {
		data->button[DS45_BTN_DIGITAL1] &= ~(DS45_CTRL_L3 | DS45_CTRL_R3);
		if (!combo_l3_r3 && s_mask_lr_frames > 0)
			s_mask_lr_frames--;
	}

	if (ps_pressed) {
		data->button[DS45_BTN_LDD]      |= DS45_CTRL_LDD_PS;
		data->button[DS45_BTN_DIGITAL1] |= DS45_CTRL_PS;
		ds45_trigger_bt_ps(1);
	} else {
		ds45_trigger_bt_ps(0);
	}

	(void)port_no;
}

/* Every poll: one raw sample into the cross-check, and - only once it has
 * PASSED - into the virtual pad. The cross-check keeps being fed after the
 * source pad disconnects, since a report that still changes then is not a
 * live one (btcheck's freeze rule). */
void passthru_poll(void)
{
	ds45_btraw_t raw;
	int i, connected, state;

#ifdef DS45_LV2DUMP
	s_dump_btn_valid = 0;
#endif
	if (!kernel_active() || s_src_port < 0)
		return;

	connected = source_still_connected(s_src_port);
	if (!connected) {
		for (i = 0; i < DS45_MAX_PADS; i++) {
			ds45_pad_t *pad = &g_pads[i];

			if (!pad->in_use || pad->link != DS45_LINK_BT)
				continue;
			ds45_boot_mark("passthru_source_disconnected");
			ldd_unregister(pad);
			pad->in_use = 0;
		}
	}

	if (read_raw(&raw) != 0)
		return;
#ifdef DS45_LV2DUMP
	if (connected) {
		s_dump_btn_valid = 1;
		s_dump_d1 = raw.d1;
		s_dump_d2 = raw.d2;
	}
#endif

	s_raw_samples++;
	bc_lock();
	state = (int)btcheck_feed_raw(&raw, connected,
	                              (uint64_t)sys_time_get_system_time());
	bc_unlock();

	if (connected && state == BTCHECK_PASSED)
		ds45_inject_bt_pad(&raw);
}

void passthru_release(void)
{
	int i;

	for (i = 0; i < DS45_MAX_PADS; i++) {
		if (g_pads[i].in_use && g_pads[i].link == DS45_LINK_BT) {
			ldd_unregister(&g_pads[i]);
			g_pads[i].in_use = 0;
		}
	}
}
