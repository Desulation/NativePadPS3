/*
 * NPP - full DualShock 4 / DualSense support for the PlayStation 3.
 *
 * Build twice from the same sources:
 *
 *   nativepadps3_vsh.sprx    loaded by the CFW plugin loader, owns the pad in
 *                            the XMB and keeps owning it while games run
 *   nativepadps3_game.sprx   loaded into a running game process (PS3MAPI or
 *                            Cobra). Corrects the stick response curve and
 *                            answers the game's cellPadGetInfo2 - see
 *                            docs/ARQUITETURA.md.
 *                            Not a rumble build: rumble is out of scope.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <sys/timer.h>
#include <cell/pad.h>

#include "npp.h"
#include "hook.h"
#ifndef NPP_GAME
#include "kernel.h"
#include "btcheck.h"
#include "kshim.h"
#ifdef NPP_DEBUG
#include <stddef.h>            /* offsetof, for the shim diagnostic trace */
#include "lv2.h"               /* npp_lv2_read */
#include "../shim/shim_abi.h"  /* npp_shim_data_t field offsets */
#endif
#endif

int npp_start(uint64_t arg);
int npp_stop(void);

SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0);
SYS_MODULE_START(npp_start);
SYS_MODULE_STOP(npp_stop);

#define THREAD_NAME      "nativepadps3"
#define STOP_THREAD_NAME "nppstop"
#define THREAD_PRIO      1000
#define THREAD_STACK     0x4000

/* How often to look for newly paired Bluetooth pads, and to refresh the
 * lightbar after a port reassignment. */
#define RESCAN_MS        1000
/* Diagnostic sampling period - must be well under a button press. */
#define DIAG_MS          20
/* How long to wait before the "loaded" notification, so it does not land
 * while the XMB is still building itself. */
#define ANNOUNCE_MS      8000

/* g_cfg lives in config.c next to its parser. */
npp_pad_t g_pads[NPP_MAX_PADS];

static sys_ppu_thread_t thread_id = (sys_ppu_thread_t)-1;
static volatile int     running;

#ifndef NPP_GAME

/* ------------------------------------------------------------------------ */
/* Rumble                                                                    */
/*                                                                           */
/* Games ask for vibration with cellPadSetActDirect(), which the firmware     */
/* routes to a physical pad we do not own. The only way to see the request is */
/* to be inside the calling process, so the in-game build detours the import  */
/* and records what the game asked for; the poll loop then forwards it to the */
/* real controller over USB.                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint8_t motor[2];    /* [0] small/high frequency, [1] large/low frequency */
	uint8_t reserved[6];
} npp_act_param_t;

static npp_detour_t  act_detour;
static uint8_t        act_small[NPP_MAX_PORT_NUM];
static uint8_t        act_large[NPP_MAX_PORT_NUM];

static int act_direct_hook(uint32_t port_no, npp_act_param_t *param)
{
	typedef int (*orig_t)(uint32_t, npp_act_param_t *);
	orig_t orig = (orig_t)(uintptr_t)act_detour.saved;

	if (param && port_no < NPP_MAX_PORT_NUM) {
		act_small[port_no] = param->motor[0];
		act_large[port_no] = param->motor[1];
	}

	return orig ? orig(port_no, param) : 0;
}

static void rumble_install(void)
{
	if (!g_cfg.rumble)
		return;

	npp_detour("sys_io", NID_CELL_PAD_SET_ACT_DIRECT,
	            (void *)act_direct_hook, &act_detour);
}

static void rumble_remove(void)
{
	npp_detour_remove(&act_detour);
}

/* ------------------------------------------------------------------------ */
/* VSH Pad Input Interception (Bluetooth Passthrough)                       */
/* ------------------------------------------------------------------------ */

static npp_opd_detour_t getdata_detour;
static npp_opd_detour_t getdataextra_detour;

static void *const keep_vsh_imports[] = {
	(void *)cellPadGetData,
	(void *)cellPadGetDataExtra,
};

static int is_bt_source_port(uint32_t port_no)
{
	int i;

	for (i = 0; i < NPP_MAX_PADS; i++) {
		if (g_pads[i].in_use && g_pads[i].link == NPP_LINK_BT &&
		    g_pads[i].handle >= 0 && g_pads[i].src_port == (int)port_no)
			return 1;
	}

	return 0;
}

static void silence_pad_data(CellPadData *data)
{
	int s;

	if (!data)
		return;

	for (s = 0; s < data->len && s < CELL_PAD_MAX_CODES; s++)
		data->button[s] = 0;

	if (data->len > NPP_BTN_ANALOG_LEFT_Y) {
		data->button[NPP_BTN_ANALOG_LEFT_X]  = NPP_STICK_CENTER;
		data->button[NPP_BTN_ANALOG_LEFT_Y]  = NPP_STICK_CENTER;
		data->button[NPP_BTN_ANALOG_RIGHT_X] = NPP_STICK_CENTER;
		data->button[NPP_BTN_ANALOG_RIGHT_Y] = NPP_STICK_CENTER;
	}

	for (s = NPP_BTN_SENSOR_X; s <= NPP_BTN_SENSOR_G && s < data->len; s++)
		data->button[s] = NPP_SENSOR_CENTER;
}

static int vsh_getdata_hook(uint32_t port_no, CellPadData *data)
{
	typedef int (*orig_t)(uint32_t, CellPadData *);
	orig_t orig = (orig_t)(uintptr_t)getdata_detour.saved;
	int r;

	if (!orig)
		return CELL_PAD_ERROR_UNINITIALIZED;

	r = orig(port_no, data);
	if (r == CELL_PAD_OK && data) {
		/* The cross-check's ground truth: what VSH itself read, before
		 * any masking of ours. */
		passthru_observe_vsh(port_no, data);
		if (data->len > 0 && port_no > 0 && is_bt_source_port(port_no))
			silence_pad_data(data);
	}

	return r;
}

static int vsh_getdataextra_hook(uint32_t port_no, uint32_t *device_type,
                                 CellPadData *data)
{
	typedef int (*orig_t)(uint32_t, uint32_t *, CellPadData *);
	orig_t orig = (orig_t)(uintptr_t)getdataextra_detour.saved;
	int r;

	if (!orig)
		return CELL_PAD_ERROR_UNINITIALIZED;

	r = orig(port_no, device_type, data);
	if (r == CELL_PAD_OK && data) {
		passthru_observe_vsh(port_no, data);
		if (data->len > 0 && port_no > 0 && is_bt_source_port(port_no))
			silence_pad_data(data);
	}

	return r;
}

static void vsh_hooks_install(void)
{
	npp_detour_opd((void *)cellPadGetData,
	                (void *)vsh_getdata_hook, &getdata_detour);
	npp_detour_opd((void *)cellPadGetDataExtra,
	                (void *)vsh_getdataextra_hook, &getdataextra_detour);
	npp_boot_mark("vsh_hooks_installed");
}

static void vsh_hooks_remove(void)
{
	npp_detour_opd_remove(&getdata_detour);
	npp_detour_opd_remove(&getdataextra_detour);
}

/* Push whatever the game last asked for to each controller. Only send when
 * something actually changed - the controllers do not need a stream of
 * identical output reports and the DualSense in particular dislikes it. */
static void rumble_update(void)
{
	int i;

	for (i = 0; i < NPP_MAX_PADS; i++) {
		npp_pad_t *pad = &g_pads[i];
		uint8_t weak, strong;

		if (!pad->in_use || pad->port < 0 || pad->port >= NPP_MAX_PORT_NUM)
			continue;

		if (!g_cfg.rumble) {
			weak = strong = 0;
		} else {
			/* The small motor is on/off on a DualShock 3; both DS4 and
			 * DualSense take a magnitude, so a request for "on" becomes
			 * full scale. */
			weak   = act_small[pad->port] ? 0xFF : 0x00;
			strong = act_large[pad->port];
		}

		if (weak == pad->out.rumble_weak && strong == pad->out.rumble_strong)
			continue;

		pad->out.rumble_weak   = weak;
		pad->out.rumble_strong = strong;
		pad->out.dirty         = 1;
	}
}

/* ------------------------------------------------------------------------ */
/* Housekeeping                                                              */
/* ------------------------------------------------------------------------ */

/* The system reshuffles ports when controllers come and go; when that
 * happens the player indicator has to follow. */
static void refresh_ports(void)
{
	int i;

	for (i = 0; i < NPP_MAX_PADS; i++) {
		npp_pad_t *pad = &g_pads[i];
		int port;

		if (!pad->in_use || pad->handle < 0)
			continue;

		port = cellPadLddGetPortNo(pad->handle);
		if (port < 0 || port == pad->port)
			continue;

		pad->port = port;
		pad->out.player = (uint8_t)((port % 4) + 1);
		pad->out.dirty  = 1;
	}
}

/* Dump every connected port once, into the boot trace rather than the debug
 * log, so it is always available without turning diagnostics on.
 *
 * This is what tells us whether the firmware's own driver still holds the
 * controller on a lower port than our virtual pad: the XMB reacts to the pad
 * on the lowest port, so a generic pad sitting on port 0 would swallow the PS
 * button no matter what we inject further up. */
static void dump_ports_once(void)
{
	static int done;
	CellPadInfo2 info;
	int port;

	if (done)
		return;
	done = 1;

	if (cellPadGetInfo2(&info) != CELL_PAD_OK) {
		npp_boot_mark("ports_getinfo2_FAILED");
		return;
	}

	npp_boot_mark_hex("ports_now_connect", info.now_connect);

	for (port = 0; port < NPP_MAX_PORT_NUM; port++) {
		if (!(info.port_status[port] & NPP_STATUS_CONNECTED))
			continue;

		/* One packed line per port: type in the high byte, capability in
		 * the low byte. type 5 = our LDD pad, 0 = a standard pad. */
		npp_boot_mark_hex(
			(port == 0) ? "port0_type_cap" :
			(port == 1) ? "port1_type_cap" :
			(port == 2) ? "port2_type_cap" :
			(port == 3) ? "port3_type_cap" :
			(port == 4) ? "port4_type_cap" :
			(port == 5) ? "port5_type_cap" : "port6_type_cap",
			(info.device_type[port] << 8) |
			(info.device_capability[port] & 0xFF));
	}
}

/* ------------------------------------------------------------------------ */
/* Shim diagnostic trace (design Amendment 3, debug VSH builds only)         */
/*                                                                           */
/* Each housekeeping tick, when the shim is installed, read its data-block    */
/* diagnostic counters (via npp_lv2_read at the base kshim placed it, plus   */
/* the ABI field offsets) and append one line to the boot trace. The file is  */
/* flushed to the HDD, so the trajectory survives a network drop and can be   */
/* read back over FTP. Nothing of this exists in release or game builds.      */
/* ------------------------------------------------------------------------ */
#ifdef NPP_DEBUG
/* One 32-bit counter: npp_lv2_read fetches the aligned qword holding it; the
 * field is the high word of the big-endian qword when its offset is 8-aligned,
 * the low word otherwise. 0 if the read refuses. */
static uint32_t shim_dbg_u32(uint64_t data_base, size_t field_off)
{
	uint64_t q;

	if (npp_lv2_read(data_base + (field_off & ~7ULL), &q))
		return 0;
	return (field_off & 4) ? (uint32_t)q : (uint32_t)(q >> 32);
}

static char *shim_dbg_tag(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *shim_dbg_hex(char *p, uint32_t v)
{
	static const char hex[] = "0123456789ABCDEF";
	int d;

	*p++ = '0';
	*p++ = 'x';
	for (d = 28; d >= 0; d -= 4)
		*p++ = hex[(v >> d) & 0xF];
	return p;
}

/* DS3-route output capture: one payload the output hook saw, as hex on its own
 * line, written only when it differs from the one last written for the same
 * capture (the pad's output reports repeat, so the trace shows each distinct
 * payload once). tag is "outcap", "outon" or "outoff"; len_off and cap_off are
 * the capture's length and 64-byte buffer in the data block; last and last_len
 * are that capture's own memory of what was last written. */
static void shim_dbg_cap_line(uint64_t base, const char *tag, size_t len_off,
                              size_t cap_off, uint8_t last[64], uint32_t *last_len)
{
	static const char hex[] = "0123456789ABCDEF";
	uint8_t cap[64];
	char line[160];   /* tag (<= 6) + " len=0x" + 8 + ' ' + 128 hex digits + '\n' = 151 */
	char *p = line;
	uint32_t len = shim_dbg_u32(base, len_off);
	uint32_t i;

	if (len > sizeof cap)
		len = sizeof cap;
	if (npp_lv2_read_bytes(base + cap_off, cap, (int)sizeof cap))
		return;
	if (len == *last_len && memcmp(cap, last, len) == 0)
		return;
	*last_len = len;
	memcpy(last, cap, sizeof cap);

	p = shim_dbg_tag(p, tag);
	p = shim_dbg_tag(p, " len=");
	p = shim_dbg_hex(p, len);
	*p++ = ' ';
	for (i = 0; i < len; i++) {
		*p++ = hex[cap[i] >> 4];
		*p++ = hex[cap[i] & 0xF];
	}
	*p++ = '\n';
	npp_append_file(NPP_BOOT_FILE, line, (int)(p - line));
}

/* The last raw DualSense report the input hook read, before translation
 * ("inraw"): its captured length, the capture counter and the bytes as hex. Written
 * only when the counter changed since the last line, so an idle pad adds nothing;
 * a busy one adds a line per housekeeping tick, not per report. It shows what the
 * pad really sends (the battery status byte is report byte 54). */
static void shim_dbg_inraw(uint64_t base)
{
	static const char hex[] = "0123456789ABCDEF";
	static uint32_t last_seq;
	uint8_t cap[64];
	char line[192];   /* "inraw len=0x" + 8 + " seq=0x" + 8 + ' ' + 128 hex digits + '\n' = 165 */
	char *p = line;
	uint32_t seq = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_raw_seq));
	uint32_t len, i;

	if (seq == last_seq)
		return;
	len = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_raw_len));
	if (len > sizeof cap)
		len = sizeof cap;
	if (npp_lv2_read_bytes(base + offsetof(npp_shim_data_t, dbg_in_raw), cap, (int)sizeof cap))
		return;
	last_seq = seq;

	p = shim_dbg_tag(p, "inraw len=");
	p = shim_dbg_hex(p, len);
	p = shim_dbg_tag(p, " seq=");
	p = shim_dbg_hex(p, seq);
	*p++ = ' ';
	for (i = 0; i < len; i++) {
		*p++ = hex[cap[i] >> 4];
		*p++ = hex[cap[i] & 0xF];
	}
	*p++ = '\n';
	npp_append_file(NPP_BOOT_FILE, line, (int)(p - line));
}

/* The last payload of any kind ("outcap"), then the last motor-on payload
 * ("outon") and the next zero-motor payload from that same pad ("outoff"). */
static void shim_dbg_outcap(uint64_t base)
{
	static uint8_t last_cap[64], last_on[64], last_off[64];
	static uint32_t len_cap, len_on, len_off;

	shim_dbg_cap_line(base, "outcap", offsetof(npp_shim_data_t, dbg_out_cap_len),
	                  offsetof(npp_shim_data_t, dbg_out_cap), last_cap, &len_cap);
	shim_dbg_cap_line(base, "outon", offsetof(npp_shim_data_t, dbg_out_on_len),
	                  offsetof(npp_shim_data_t, dbg_out_on_cap), last_on, &len_on);
	shim_dbg_cap_line(base, "outoff", offsetof(npp_shim_data_t, dbg_out_off_len),
	                  offsetof(npp_shim_data_t, dbg_out_off_cap), last_off, &len_off);
}

/* Power-off hook counters ("podbg"): close-send hook entries, calls for a
 * DualSense, calls rewritten, calls passed through, teardown hook entries, and
 * the power-off stage the plugin set. Written once on the first call, then only
 * when one of them changed since the last line, so an idle console adds nothing. */
static void shim_dbg_podbg(uint64_t base)
{
	static uint32_t last[6];
	static int primed;
	uint32_t v[6];
	char line[112];   /* "podbg" + 6 tags (" calls=", " ds=", " rew=", " pass=", " td=", " mode=" = 32) + 6 * 10 + '\n' = 98 */
	char *p = line;

	v[0] = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_po_calls));
	v[1] = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_po_ds));
	v[2] = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_po_rewritten));
	v[3] = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_po_passed));
	v[4] = shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_td_calls));
	v[5] = shim_dbg_u32(base, offsetof(npp_shim_data_t, po_mode));
	/* The first call always writes: in observe mode every value is still zero at
	 * install, so a compare against the zeroed static would print nothing until
	 * the first hook event, and "the line appeared" is the install check. */
	if (primed && memcmp(v, last, sizeof v) == 0)
		return;
	primed = 1;
	memcpy(last, v, sizeof v);

	p = shim_dbg_tag(p, "podbg calls=");
	p = shim_dbg_hex(p, v[0]);
	p = shim_dbg_tag(p, " ds=");
	p = shim_dbg_hex(p, v[1]);
	p = shim_dbg_tag(p, " rew=");
	p = shim_dbg_hex(p, v[2]);
	p = shim_dbg_tag(p, " pass=");
	p = shim_dbg_hex(p, v[3]);
	p = shim_dbg_tag(p, " td=");
	p = shim_dbg_hex(p, v[4]);
	p = shim_dbg_tag(p, " mode=");
	p = shim_dbg_hex(p, v[5]);
	*p++ = '\n';
	npp_append_file(NPP_BOOT_FILE, line, (int)(p - line));
}

/* The power-off event ring ("poevt"): one line per record newer than the last
 * one printed, oldest first, as the sequence and the 16 record bytes in hex. The
 * ring holds the last eight calls, so at most eight lines per read; a gap larger
 * than that loses the oldest, which the sequence numbers make visible. A record's
 * sequence is its first four bytes, big-endian; an empty slot has sequence 0 and is
 * skipped. "Newer" is a signed 32-bit difference, so the counter wrapping past
 * 0xFFFFFFFF still orders correctly. */
static void shim_dbg_poevt(uint64_t base)
{
	static const char hex[] = "0123456789ABCDEF";
	static uint32_t last_seq;
	uint8_t ring[128];
	uint8_t done[8] = {0};
	int n, s, best, j;

	if (npp_lv2_read_bytes(base + offsetof(npp_shim_data_t, po_ring), ring, (int)sizeof ring))
		return;

	for (n = 0; n < 8; n++) {
		uint32_t bestseq = 0, bestd = 0;
		char line[64];   /* "poevt seq=" + 10 + ' ' + 32 hex digits + '\n' = 54 */
		char *p = line;

		best = -1;
		for (s = 0; s < 8; s++) {
			const uint8_t *r = ring + 16 * s;
			uint32_t seq = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
			               ((uint32_t)r[2] << 8) | r[3];
			uint32_t d = seq - last_seq;

			if (done[s] || seq == 0 || (int32_t)d <= 0)
				continue;
			if (best < 0 || d < bestd) {
				best = s;
				bestseq = seq;
				bestd = d;
			}
		}
		if (best < 0)
			break;
		done[best] = 1;
		last_seq = bestseq;

		p = shim_dbg_tag(p, "poevt seq=");
		p = shim_dbg_hex(p, bestseq);
		*p++ = ' ';
		for (j = 0; j < 16; j++) {
			*p++ = hex[ring[16 * best + j] >> 4];
			*p++ = hex[ring[16 * best + j] & 0xF];
		}
		*p++ = '\n';
		npp_append_file(NPP_BOOT_FILE, line, (int)(p - line));
	}
}

static void shim_dbg_trace(void)
{
	uint64_t base = 0, vt = 0;
	/* The longest line is 425 bytes: 25 tags (164 bytes with their spaces and
	 * '='), 26 values of "0x" and 8 hex digits (260 bytes; lastvt is two of them)
	 * and the newline. 448 leaves margin. */
	char line[448];
	char *p = line;

	if (!kshim_dbg_data_base(&base))
		return;

	p = shim_dbg_tag(p, "shimdbg ev=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_event_calls)));
	p = shim_dbg_tag(p, " en=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_enable_calls)));
	p = shim_dbg_tag(p, " in=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_input_calls)));
	p = shim_dbg_tag(p, " inds=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_input_ds)));
	p = shim_dbg_tag(p, " padmiss=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_input_padmiss)));
	p = shim_dbg_tag(p, " comp=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, stat_completed)));
	p = shim_dbg_tag(p, " en2=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, stat_enabled)));
	p = shim_dbg_tag(p, " xl=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, stat_translated)));
	p = shim_dbg_tag(p, " ps=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, stat_passed)));
	p = shim_dbg_tag(p, " lastpid=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_last_pid)));
	p = shim_dbg_tag(p, " lastvt=");
	(void)npp_lv2_read(base + offsetof(npp_shim_data_t, dbg_last_vtable), &vt);
	p = shim_dbg_hex(p, (uint32_t)(vt >> 32));
	p = shim_dbg_hex(p, (uint32_t)vt);
	/* stage 1b: identities taken from a Bluetooth address, completions that fell
	 * back to the counter, and the last connection handle looked up */
	p = shim_dbg_tag(p, " idbd=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_id_bd)));
	p = shim_dbg_tag(p, " idfb=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_id_fallback)));
	p = shim_dbg_tag(p, " hnd=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_last_handle)));
	/* DS3-route output hook: calls entered, calls for a DualSense, calls rewritten,
	 * calls passed through unchanged, and the last call's payload length */
	p = shim_dbg_tag(p, " outc=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_calls)));
	p = shim_dbg_tag(p, " outds=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_ds)));
	p = shim_dbg_tag(p, " outxl=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_translated)));
	p = shim_dbg_tag(p, " outps=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_passed)));
	p = shim_dbg_tag(p, " outlen=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_last_len)));
	/* rumble-aware capture: motor-on payloads seen, and zero-motor payloads that
	 * followed one from the same pad */
	p = shim_dbg_tag(p, " outon=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_on_count)));
	p = shim_dbg_tag(p, " outoff=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_out_off_count)));
	/* DS3-route input (the full report): reads of a DualSense through the bounce
	 * buffer, reads of 66-79 bytes, results dropped for not fitting the caller's
	 * buffer, and errors */
	p = shim_dbg_tag(p, " inb=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_bounce)));
	p = shim_dbg_tag(p, " inbig=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_big)));
	p = shim_dbg_tag(p, " indrop=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_drop)));
	p = shim_dbg_tag(p, " inerr=");
	p = shim_dbg_hex(p, shim_dbg_u32(base, offsetof(npp_shim_data_t, dbg_in_err)));
	*p++ = '\n';

	npp_append_file(NPP_BOOT_FILE, line, (int)(p - line));
	shim_dbg_outcap(base);
	shim_dbg_inraw(base);
	shim_dbg_podbg(base);
	shim_dbg_poevt(base);
}
#endif /* NPP_DEBUG */

/* ------------------------------------------------------------------------ */
/* Bluetooth kernel support                                                  */
/* ------------------------------------------------------------------------ */

#define TOAST_BT_UNSUPPORTED "NativePadPS3: Bluetooth not supported on this kernel"

/* No usable kernel record, or the cross-check rejected the one we had. USB
 * mode never reads the Bluetooth report, so it never reports this. */
static int bt_unsupported(void)
{
	if (g_cfg.mode == NPP_MODE_USB)
		return 0;
	return kernel_active_source() == NPP_KMATCH_NONE ||
	       passthru_btcheck_state() == BTCHECK_FAILED;
}

#endif /* !NPP_GAME */

static void pads_reset(void)
{
	int i;

	memset(g_pads, 0, sizeof(g_pads));
	for (i = 0; i < NPP_MAX_PADS; i++) {
		g_pads[i].handle   = -1;
		g_pads[i].port     = -1;
		g_pads[i].src_port = -1;
		g_pads[i].dev_id   = -1;
		g_pads[i].c_pipe   = -1;
		g_pads[i].i_pipe   = -1;
		g_pads[i].o_pipe   = -1;
	}
}

static void npp_thread(uint64_t arg)
{
#ifdef NPP_GAME
	uint32_t game_tick = 0;
#else
	uint32_t since_rescan = 0;
	uint32_t since_diag = 0;
	uint32_t uptime = 0;
	int announced = 0;
	int bt_toast_shown = 0;
	int last_btcheck = -1;
	int usb_ok;
#ifdef NPP_LV2DUMP
	lv2dump_t lv2dump = { 0, 0, 0, 0, 0, 0 };
	static lv2capture_t lv2capture;   /* zeroed; off the 16 KiB stack */
#endif
#endif

	(void)arg;

	/* Startup is traced to a file, one marker written *before* each step that
	 * touches the system. If the console ever hangs while loading the plugin,
	 * the last marker in that file names the step that did it. */
	npp_boot_mark("start");
	npp_boot_mark(NPP_VERSION);
	npp_paths_init();

	npp_config_defaults(&g_cfg);
	npp_config_load(&g_cfg, npp_path(NPP_PATH_CONFIG));
	npp_boot_mark("config");

	if (!g_cfg.enable) {
		/* Loaded, and deliberately inert. Nothing below this point runs, so
		 * if the console still misbehaves the module itself is not the
		 * cause. */
		npp_boot_mark("DISABLED_by_config");
		sys_ppu_thread_exit(0);
		return;
	}

	pads_reset();
	sticks_load();
	npp_boot_mark("sticks");

#ifdef NPP_GAME
	/* Inside a game process the job is different and much smaller: correct the
	 * stick curve on the way to the game and touch nothing else. No virtual
	 * pad, so no device-type problem - see gamehook.c.
	 *
	 * Nothing here may touch libusbd. A game process does not load it - the
	 * module list of a running title has libio and libfs but no libusbd - so
	 * an import of it leaves the loader unable to resolve the module, and
	 * PS3MAPI just sits there loading forever. */
	npp_boot_mark("game_mode");

	/* Slot 0 holds the calibration the game hook corrects against. Claim it:
	 * sticks_save_if_dirty() only considers slots in use, and in this build
	 * nothing else ever claims one - there is no USB or Bluetooth attach path
	 * here - so without this the calibration is learned every session and
	 * written none of them. */
	g_pads[0].in_use = 1;

	gamehook_install();

	while (running) {
		sys_timer_usleep(200 * 1000);

		/* Five ticks to the second, matching what the rate limit inside
		 * sticks_save_if_dirty() assumes. */
		if (g_cfg.stick_calibrate && ++game_tick >= 5) {
			game_tick = 0;
			sticks_save_if_dirty(0);
		}
	}

	gamehook_remove();

	/* Restoring the descriptor does not evict a game thread already executing
	 * inside the hook, and the module is freed as soon as npp_stop() returns.
	 * Without a pause here, unloading the plugin while the game is polling the
	 * pad can leave a thread running in memory that has just been released. */
	sys_timer_usleep(300 * 1000);

	if (g_cfg.stick_calibrate)
		sticks_save_if_dirty(1);
	sys_ppu_thread_exit(0);

#else

	/* No cellSysmoduleLoadModule() here on purpose: libusbd is already up in
	 * the VSH process, and importing sysmodule pulls a stub the VSH may not
	 * resolve at load time - which hangs the console instead of failing. */
	if (g_cfg.mode != NPP_MODE_BT) {
		npp_boot_mark("usb_init");
		usb_ok = (usb_init() == 0);
	} else {
		usb_ok = 0;
	}
	npp_boot_mark(usb_ok ? "usb_init_OK" : "usb_inactive_or_failed");

	rumble_install();
	npp_boot_mark("rumble");

	/* The cross-check lock must exist before the detours can call in. */
	passthru_init();
	vsh_hooks_install();

	/* Where this kernel keeps the Bluetooth report: once, before the loop.
	 * A pad that connects meanwhile is adopted on the first rescan after. */
	kernel_log_fw();
	if (g_cfg.mode != NPP_MODE_USB) {
		npp_kmatch_t kmatch;
		kshim_result_t kshim;
		int probe_ran = 0;

		npp_boot_mark("kernel_resolve");
		kmatch = kernel_resolve(NULL);
		npp_boot_mark("kernel_resolve_done");
		NPP_TRACE_HEX("kmatch", kmatch);
		(void)kmatch;                /* release: the trace compiles away */

		/* On a fingerprinted kernel, install the DS3-route shim so a DualSense
		 * runs through the PS3's own Sony-pad path. Entirely RAM-only, verified
		 * and read back; a mismatch or a failure writes nothing and the pad
		 * keeps today's generic behaviour. The result (and, in debug builds,
		 * the failing step) is traced.
		 *
		 * Gated by ENABLE_SHIM, which is off by default: the shim patches live
		 * kernel code, so a freshly deployed build never does that on its own and
		 * a bad one cannot hang the console at boot. The shim goes in only when
		 * nativepadps3.txt says ENABLE_SHIM = 1; to recover from a hang, remove that
		 * line and reboot. With the gate shut the kernel is not touched at all
		 * (kshim_install() refuses by itself as well). */
		/* Executability pre-flight (design Amendment 2, console step 0): with
		 * PROBE_EXEC set, suppress the shim install this boot in EVERY VSH build,
		 * so the pre-flight can never co-exist with a no-preflight install. A
		 * debug build additionally runs the probe - proving the fixed executable
		 * region runs one leaf stub on the plugin's own one-shot syscall thread
		 * (the smallest possible blast radius) before any hook is installed, on a
		 * fingerprint-matched row, exactly where the shim would otherwise go. */
		if (g_cfg.probe_exec) {
			probe_ran = 1;
#ifdef NPP_DEBUG
			{
				const npp_shim_kernel_t *ek = NULL;
				uint64_t exec_out = 0;
				int ei;

				for (ei = 0; ei < npp_shim_table_count; ei++)
					if (npp_shim_table[ei].fp_addr != 0 &&
					    kernel_fingerprint_matches(npp_shim_table[ei].fp_addr,
					                               npp_shim_table[ei].fp_hash)) {
						ek = &npp_shim_table[ei];
						break;
					}
				npp_boot_mark("exec_probe");
				if (ek) {
					int pr = kshim_exec_probe(ek, &exec_out);

					if (pr == 0)
						NPP_TRACE_HEX("exec_probe", exec_out);
					else
						NPP_TRACE_HEX("exec_probe_FAILED", (uint64_t)pr);
				} else {
					NPP_TRACE("exec_probe_no_entry");
				}
			}
#endif
		}
		if (!probe_ran && g_cfg.enable_shim) {
			npp_boot_mark("kshim_install");
			kshim = kshim_install();
			NPP_TRACE_HEX("kshim", kshim);
			(void)kshim;             /* release: the trace compiles away */
		} else if (!probe_ran) {
			NPP_TRACE("kshim_gated_off");
		}
	}

	if (g_cfg.diag) {
		diag_msg("=== NativePadPS3 " NPP_VERSION " ===");
		diag_dump_ports();
	}

	npp_boot_mark("loop");
	while (running) {
		sys_timer_usleep(g_cfg.poll_ms * 1000);

		if (usb_ok)
			usb_poll();

		if (g_cfg.mode != NPP_MODE_USB)
			passthru_poll();

		rumble_update();

		/* Announce once the XMB has had time to come up. Waiting here rather
		 * than before the loop keeps controllers alive from the first tick. */
		uptime += g_cfg.poll_ms;
		if (!announced && uptime >= ANNOUNCE_MS) {
			announced = 1;
			if (g_cfg.notify) {
				npp_boot_mark("notify");
				if (bt_unsupported()) {
					bt_toast_shown = 1;
					npp_notify(TOAST_BT_UNSUPPORTED);
				} else {
					npp_notify("NativePadPS3 Caricato / Loaded");
				}
				npp_boot_mark("notify_ok");
			}
		}

#ifdef NPP_LV2DUMP
		/* Debug builds, DUMP_LV2 = 1: after the XMB is up, two dumps once a
		 * Bluetooth pad is connected. Each holds this thread for a few
		 * seconds while 8 MiB are read through syscall 6. */
		if (announced && g_cfg.dump_lv2)
			lv2dump_poll(&lv2dump, uptime);

		/* CAPTURE_LV2 / CAPTURE_LEN: every tick from the first, so a pad
		 * connecting during boot is caught too. */
		lv2capture_poll(&lv2capture, uptime);
#endif

		/* Fast enough to catch a button press, slow enough not to drown the
		 * log or the console in file writes. */
		since_diag += g_cfg.poll_ms;
		if (g_cfg.diag && since_diag >= DIAG_MS) {
			since_diag = 0;
			diag_watch();
		}

		since_rescan += g_cfg.poll_ms;
		if (since_rescan >= RESCAN_MS) {
			since_rescan = 0;
			if (g_cfg.diag)
				diag_flush();

			refresh_ports();
			dump_ports_once();
			if (usb_ok)
				usb_report_probes();

#ifdef NPP_DEBUG
			/* The shim's diagnostic counters to the HDD trace (a no-op unless
			 * the shim is installed), so the trajectory survives a network
			 * drop (design Amendment 3). */
			shim_dbg_trace();
#endif

			/* Persist whatever the sticks have taught us. Rate limited
			 * inside, so this is a no-op most of the time. */
			if (g_cfg.stick_calibrate)
				sticks_save_if_dirty(0);

			/* In AUTO mode a cabled controller always wins: only look for
			 * Bluetooth pads when nothing is attached over USB. Not gated on
			 * capacity: a bound source pad waits here for the cross-check
			 * before it is registered, and passthru_scan binds at most one. */
			if (g_cfg.mode == NPP_MODE_BT ||
			    (g_cfg.mode == NPP_MODE_AUTO && npp_usb_pads_active() == 0))
				passthru_scan();

			if (g_cfg.mode != NPP_MODE_USB) {
				int st = passthru_btcheck_state();

				NPP_TRACE_HEX("btcheck_vsh_samples",
				               passthru_btcheck_vsh_samples());
#ifdef NPP_DEBUG
				passthru_trace_vsh_reads();
#endif
				if (st != last_btcheck) {
					last_btcheck = st;
					NPP_TRACE_HEX("btcheck", st);
				}
				/* A cross-check that fails after the announcement still
				 * gets its toast, once. */
				if (announced && !bt_toast_shown && g_cfg.notify &&
				    bt_unsupported()) {
					bt_toast_shown = 1;
					npp_notify(TOAST_BT_UNSUPPORTED);
				}
			}
		}
	}

	if (g_cfg.stick_calibrate)
		sticks_save_if_dirty(1);

	vsh_hooks_remove();
	rumble_remove();
	passthru_release();
	passthru_shutdown();
	if (usb_ok)
		usb_shutdown();
	diag_flush();

	sys_ppu_thread_exit(0);
#endif /* NPP_GAME */
}

/* ------------------------------------------------------------------------ */
/* PRX entry points                                                          */
/* ------------------------------------------------------------------------ */

static inline void thread_exit(uint64_t val)
{
	system_call_1(41, val);
}

int npp_start(uint64_t arg)
{
	int result;
	(void)arg;

	running = 1;
	result = sys_ppu_thread_create(&thread_id, npp_thread, 0, THREAD_PRIO,
	                      THREAD_STACK, SYS_PPU_THREAD_CREATE_JOINABLE,
	                      THREAD_NAME);
	if (result != 0) {
		running = 0;
		return SYS_PRX_NO_RESIDENT;
	}

	thread_exit(0);
	return SYS_PRX_RESIDENT;
}

static void npp_stop_thread(uint64_t arg)
{
	uint64_t exit_code;

	(void)arg;

	running = 0;
	sys_timer_usleep(300 * 1000);

	if (thread_id != (sys_ppu_thread_t)-1)
		sys_ppu_thread_join(thread_id, &exit_code);

#ifndef NPP_GAME
	/* Unload the kernel shim in two steps. kshim_unhook() switches the redirects
	 * and product-ID words back so the DS3 driver stops calling into the blob
	 * (and arms the scrub only if that reverting actually stuck). Then drain: the
	 * fixed blob region cannot be freed, so a thread already inside the bounded,
	 * straight-line blob code must be given time to run out before kshim_scrub()
	 * zeroes it - zeroing live code would fault the kernel. Both are a no-op when
	 * nothing was installed (USB mode, ENABLE_SHIM off, or an unmatched kernel). */
	kshim_unhook();
	sys_timer_usleep(150 * 1000);
	kshim_scrub();

	/* Never in the game build. npp_notify() resolves vshtask by walking the
	 * table at 0x1008C + 0x984, which only *is* an import table in the VSH
	 * process. In a game process that address holds arbitrary EBOOT data being
	 * read as import stubs: every pointer taken from it is range checked, but
	 * a plausible address is not necessarily a mapped one, so the walk can
	 * fault. A notification cannot reach the screen from a game anyway. */
	if (g_cfg.notify)
		npp_notify("NativePadPS3 Disattivato / Unloaded");
#endif
	sys_ppu_thread_exit(0);
}

int npp_stop(void)
{
	sys_ppu_thread_t t;
	uint64_t exit_code;

	sys_ppu_thread_create(&t, npp_stop_thread, 0, THREAD_PRIO,
	                      THREAD_STACK, SYS_PPU_THREAD_CREATE_JOINABLE,
	                      STOP_THREAD_NAME);
	sys_ppu_thread_join(t, &exit_code);

	thread_exit(0);
	return SYS_PRX_STOP_OK;
}
