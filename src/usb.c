/*
 * NPP - USB transport.
 *
 * Requests access using cellUsbdRegisterExtraLdd(). Successful registration
 * does not prove ownership: only attach followed by valid reports does.
 *
 * The USB completion callback copies the completed report into a bounded
 * queue and resubmits. Parsing happens on the injection thread.
 */

#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/prx.h>
#include <sys/synchronization.h>
#include <sys/timer.h>
#include <cell/usbd.h>

#include "npp.h"

#define USB_CLASS_HID       0x03
#define MAX_DEVICE_ENTRIES  16

typedef struct {
	uint16_t     vid;
	uint16_t     pid;
	npp_model_t model;
	const char  *name;
} npp_device_t;

/* Known controllers. Additional ids can be appended at runtime from
 * nativepadps3_devices.txt without rebuilding. */
static npp_device_t devices[MAX_DEVICE_ENTRIES] = {
	{ NPP_VID_SONY, NPP_PID_DS4_V1,         NPP_MODEL_DS4,       "DualShock 4 (CUH-ZCT1)" },
	{ NPP_VID_SONY, NPP_PID_DS4_V2,         NPP_MODEL_DS4,       "DualShock 4 (CUH-ZCT2)" },
	{ NPP_VID_SONY, NPP_PID_DS4_DONGLE,     NPP_MODEL_DS4,       "DualShock 4 USB adaptor" },
	{ NPP_VID_SONY, NPP_PID_DUALSENSE,      NPP_MODEL_DUALSENSE, "DualSense" },
	{ NPP_VID_SONY, NPP_PID_DUALSENSE_EDGE, NPP_MODEL_DUALSENSE, "DualSense Edge" },
};
static int device_count = 5;

static int32_t npp_probe(int32_t dev_id);
static int32_t npp_attach(int32_t dev_id);
static int32_t npp_detach(int32_t dev_id);

static const char *op_names[MAX_DEVICE_ENTRIES] = {
	"npp_0", "npp_1", "npp_2", "npp_3", "npp_4",
	"npp_5", "npp_6", "npp_7", "npp_8", "npp_9",
	"npp_10", "npp_11", "npp_12", "npp_13", "npp_14", "npp_15"
};
static CellUsbdLddOps npp_ops[MAX_DEVICE_ENTRIES];

static sys_mutex_t usb_mutex;
static int         usb_ready;

#define SWAP16(x) ((uint16_t)((((x) & 0x00FF) << 8) | (((x) & 0xFF00) >> 8)))

static void lock(void)   { if (usb_ready) sys_mutex_lock(usb_mutex, 0); }
static void unlock(void) { if (usb_ready) sys_mutex_unlock(usb_mutex); }

static const npp_device_t *lookup(uint16_t vid, uint16_t pid)
{
	int i;

	for (i = 0; i < device_count; i++)
		if (devices[i].vid == vid && devices[i].pid == pid)
			return &devices[i];

	return NULL;
}

static npp_pad_t *alloc_slot(void)
{
	int i;

	/* Skip slots that still hold a virtual pad awaiting cleanup, or handing
	 * one out here would overwrite the handle and leak it. */
	for (i = 0; i < NPP_MAX_PADS; i++)
		if (!g_pads[i].in_use && g_pads[i].handle < 0)
			return &g_pads[i];

	return NULL;
}

static npp_pad_t *slot_for_dev(int32_t dev_id)
{
	int i;

	for (i = 0; i < NPP_MAX_PADS; i++)
		if (g_pads[i].in_use && g_pads[i].link == NPP_LINK_USB &&
		    g_pads[i].dev_id == dev_id)
			return &g_pads[i];

	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Transfers                                                                 */
/* ------------------------------------------------------------------------ */

static void submit_in(npp_pad_t *pad);

static void in_done(int32_t result, int32_t count, void *arg)
{
	npp_pad_t *pad = (npp_pad_t *)arg;

	lock();
	if (!pad->in_use) {
		unlock();
		return;
	}

	if (result == 0 && count > 0) {
		npp_report_push(&pad->reports, pad->buf[pad->buf_idx], count);
		pad->buf_idx ^= 1;
	} else if (result != 0) {
		pad->usb_error = result;
	}
	unlock();

	submit_in(pad);
}

static void submit_in(npp_pad_t *pad)
{
	int result;
	if (!pad->in_use || pad->i_pipe < 0)
		return;
	result = cellUsbdInterruptTransfer(pad->i_pipe, pad->buf[pad->buf_idx],
	                                  pad->in_size, in_done, pad);
	if (result != 0)
		pad->usb_error = result;
}

static void out_done(int32_t result, int32_t count, void *arg)
{
	(void)result; (void)count; (void)arg;
}

void usb_send_output(npp_pad_t *pad)
{
	int len;

	if (pad->link != NPP_LINK_USB || pad->o_pipe < 0)
		return;

	if (pad->model == NPP_MODEL_DUALSENSE)
		len = dualsense_build_output(pad->obuf, sizeof(pad->obuf),
		                             NPP_LINK_USB, &pad->out);
	else
		len = ds4_build_output(pad->obuf, sizeof(pad->obuf),
		                       NPP_LINK_USB, &pad->out);

	if (len > 0)
		cellUsbdInterruptTransfer(pad->o_pipe, pad->obuf, len, out_done, pad);

	pad->out.dirty = 0;
}

static void config_done(int32_t result, int32_t count, void *arg)
{
	npp_pad_t *pad = (npp_pad_t *)arg;
	(void)count;
	if (result != 0) {
		pad->usb_error = result;
		return;
	}
	submit_in(pad);
}

/* ------------------------------------------------------------------------ */
/* Enumeration                                                               */
/* ------------------------------------------------------------------------ */

/* Probe results, recorded in memory only.
 *
 * probe() runs on the USB manager's thread, so it must not write files - that
 * is what was blocking app startup. The polling thread drains this instead. */
#define PROBE_LOG_MAX 8
static volatile uint16_t probe_vid[PROBE_LOG_MAX];
static volatile uint16_t probe_pid[PROBE_LOG_MAX];
static volatile uint8_t  probe_hit[PROBE_LOG_MAX];
static volatile int      probe_n;
static int               probe_reported;
static int               probe_empty_ticks;

static int32_t npp_probe(int32_t dev_id)
{
	UsbDeviceDescriptor *ddesc;
	uint16_t vid, pid;
	int matched;

	ddesc = (UsbDeviceDescriptor *)
	        cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
	if (!ddesc)
		return CELL_USBD_PROBE_FAILED;

	vid = SWAP16(ddesc->idVendor);
	pid = SWAP16(ddesc->idProduct);
	matched = (lookup(vid, pid) != NULL);

	if (probe_n < PROBE_LOG_MAX) {
		int n = probe_n;

		probe_vid[n] = vid;
		probe_pid[n] = pid;
		probe_hit[n] = (uint8_t)matched;
		probe_n = n + 1;
	}

	return matched ? CELL_USBD_PROBE_SUCCEEDED : CELL_USBD_PROBE_FAILED;
}

/* Called from the polling thread: says whether the USB manager ever offered us
 * the controller at all. No probe line means the firmware never handed the
 * device to userland USB, and no amount of VID/PID registration will help. */
void usb_report_probes(void)
{
	int i, n = probe_n;

	if (n == 0) {
		if (++probe_empty_ticks == 10)
			npp_boot_mark("usb_no_probe_first_10s_replug_to_test");
		return;
	}

	for (i = probe_reported; i < n; i++)
		npp_boot_mark_hex(probe_hit[i] ? "probe_MATCH_vidpid"
		                               : "probe_seen_vidpid",
		                   ((uint32_t)probe_vid[i] << 16) | probe_pid[i]);
	probe_reported = n;
}

static int32_t npp_attach(int32_t dev_id)
{
	UsbDeviceDescriptor        *ddesc;
	UsbConfigurationDescriptor *cdesc;
	UsbInterfaceDescriptor     *idesc;
	UsbEndpointDescriptor      *ep_in = NULL, *ep_out = NULL;
	const npp_device_t        *dev;
	npp_pad_t                 *pad;
	uint8_t                    *desc;
	int                         in_hid = 0, result;

	ddesc = (UsbDeviceDescriptor *)
	        cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);
	if (!ddesc)
		return CELL_USBD_ATTACH_FAILED;

	dev = lookup(SWAP16(ddesc->idVendor), SWAP16(ddesc->idProduct));
	if (!dev)
		return CELL_USBD_ATTACH_FAILED;

	cdesc = (UsbConfigurationDescriptor *)
	        cellUsbdScanStaticDescriptor(dev_id, NULL,
	                                     USB_DESCRIPTOR_TYPE_CONFIGURATION);
	if (!cdesc)
		return CELL_USBD_ATTACH_FAILED;

	/* Both controllers expose audio interfaces alongside the gamepad, and the
	 * HID interface number is not the same across models, so walk the whole
	 * descriptor chain and take the endpoints that belong to the HID one. */
	desc = (uint8_t *)cdesc;
	while ((desc = cellUsbdScanStaticDescriptor(dev_id, desc, 0)) != NULL) {
		if (desc[1] == USB_DESCRIPTOR_TYPE_INTERFACE) {
			if (ep_in)
				break;
			idesc  = (UsbInterfaceDescriptor *)desc;
			in_hid = (idesc->bInterfaceClass == USB_CLASS_HID);
			ep_in = ep_out = NULL;
		} else if (desc[1] == USB_DESCRIPTOR_TYPE_ENDPOINT && in_hid) {
			UsbEndpointDescriptor *e = (UsbEndpointDescriptor *)desc;

			if ((e->bmAttributes & 0x03) != 0x03)  /* interrupt only */
				continue;

			if (e->bEndpointAddress & 0x80) {
				if (!ep_in)  ep_in  = e;
			} else {
				if (!ep_out) ep_out = e;
			}

			if (ep_in && ep_out)
				break;
		}
	}

	if (!ep_in)
		return CELL_USBD_ATTACH_FAILED;

	lock();

	/* Both controllers expose audio interfaces next to the gamepad, and the
	 * framework can call attach more than once for the same device. Without
	 * this check each call built another slot and registered another virtual
	 * pad, leaving two of them fighting over the same physical controller. */
	if (slot_for_dev(dev_id)) {
		unlock();
		return CELL_USBD_ATTACH_SUCCEEDED;
	}

	/* In AUTO mode: if a Bluetooth pad is already active on the console,
	 * only preempt it if the Bluetooth source pad is parked on a secondary port
	 * (e.g. Controller 7 / Port 6), which guarantees Port 0 is free for the USB
	 * controller without double input. If Bluetooth is on Port 0, skip USB attach
	 * so it charges safely over USB without conflict. */
	if (g_cfg.mode == NPP_MODE_AUTO && npp_bt_pads_active() > 0) {
		int can_preempt = 1;
		int j;
		for (j = 0; j < NPP_MAX_PADS; j++) {
			if (g_pads[j].in_use && g_pads[j].link == NPP_LINK_BT && g_pads[j].src_port == 0) {
				can_preempt = 0;
				break;
			}
		}
		if (can_preempt) {
			npp_boot_mark("usb_preempt_bt_clean");
			passthru_release();
		} else {
			npp_boot_mark("usb_attach_skip_bt_active");
			unlock();
			return CELL_USBD_ATTACH_SUCCEEDED;
		}
	}

	pad = alloc_slot();
	if (!pad) {
		unlock();
		return CELL_USBD_ATTACH_FAILED;
	}

	/* Clearing the slot must not take the stick calibration with it - that
	 * was loaded from disk at startup, and a zeroed calibration leaves the
	 * axes with a span of one unit, which turns half of every stick into an
	 * on/off switch. */
	{
		npp_stickcal_t keep = pad->cal;

		memset(pad, 0, sizeof(*pad));
		pad->cal = keep;
	}

	pad->in_use  = 1;
	pad->model   = dev->model;
	pad->link    = NPP_LINK_USB;
	pad->dev_id  = dev_id;
	pad->handle  = -1;
	pad->port    = -1;
	pad->src_port = -1;
	pad->c_pipe  = pad->i_pipe = pad->o_pipe = -1;
	pad->in_size = SWAP16(ep_in->wMaxPacketSize);
	if (pad->in_size > (int)sizeof(pad->buf[0]))
		pad->in_size = (int)sizeof(pad->buf[0]);

	pad->c_pipe = cellUsbdOpenPipe(dev_id, NULL);
	if (pad->c_pipe < 0)
		goto fail;

	pad->i_pipe = cellUsbdOpenPipe(dev_id, ep_in);
	if (pad->i_pipe < 0)
		goto fail;

	if (ep_out) {
		pad->o_pipe   = cellUsbdOpenPipe(dev_id, ep_out);
		pad->out_size = SWAP16(ep_out->wMaxPacketSize);
	}

	cellUsbdSetPrivateData(dev_id, pad);
	unlock();

	result = cellUsbdSetConfiguration(pad->c_pipe, cdesc->bConfigurationValue,
	                                 config_done, pad);
	if (result != 0) {
		pad->usb_error = result;
		/* No callback was scheduled. Release the pipes opened above. */
		lock();
		goto fail;
	}

	/* Hand the virtual pad registration to the polling thread and get out of
	 * the USB manager's way - see the comment on needs_register. */
	pad->needs_register = 1;

	return CELL_USBD_ATTACH_SUCCEEDED;

fail:
	if (pad->i_pipe >= 0) cellUsbdClosePipe(pad->i_pipe);
	if (pad->o_pipe >= 0) cellUsbdClosePipe(pad->o_pipe);
	if (pad->c_pipe >= 0) cellUsbdClosePipe(pad->c_pipe);
	pad->i_pipe = pad->o_pipe = pad->c_pipe = -1;
	pad->in_use = 0;
	unlock();
	return CELL_USBD_ATTACH_FAILED;
}

static int32_t npp_detach(int32_t dev_id)
{
	npp_pad_t *pad;

	lock();
	pad = slot_for_dev(dev_id);
	if (!pad) {
		unlock();
		return CELL_USBD_DETACH_FAILED;
	}

	/* Only mark it dead here. Unregistering the virtual pad is a libpad call
	 * and belongs on the polling thread, not in the USB manager's callback. */
	pad->in_use = 0;
	pad->needs_register = 0;
	pad->i_pipe = pad->o_pipe = pad->c_pipe = -1;
	unlock();

	return CELL_USBD_DETACH_SUCCEEDED;
}

/* ------------------------------------------------------------------------ */
/* Poll: parse whatever the callback last landed and push it to the pad       */
/* ------------------------------------------------------------------------ */

void usb_poll(void)
{
	int i;

	for (i = 0; i < NPP_MAX_PADS; i++) {
		npp_pad_t *pad = &g_pads[i];
		unsigned remaining;

		/* Deferred cleanup for controllers unplugged since the last pass. */
		if (!pad->in_use && pad->handle >= 0) {
			ldd_unregister(pad);
			if (g_cfg.mode == NPP_MODE_AUTO && npp_usb_pads_active() == 0) {
				npp_boot_mark("usb_detached_scanning_bt");
				passthru_scan();
			}
			continue;
		}

		if (!pad->in_use || pad->link != NPP_LINK_USB)
			continue;
		if (pad->usb_error && pad->usb_error != pad->usb_error_reported) {
			npp_boot_mark_hex("usb_transfer_FAILED", (uint32_t)pad->usb_error);
			pad->usb_error_reported = pad->usb_error;
		}

		/* Deferred from the attach callback, so the slow parts happen here. */
		if (pad->needs_register) {
			pad->needs_register = 0;
			npp_boot_mark("usb_attached");
			if (ldd_register(pad) == 0)
				usb_send_output(pad);
		}

		for (remaining = NPP_REPORT_QUEUE_SIZE; remaining; remaining--) {
			uint8_t buf[80];
			int len, result, overflow;
			lock();
			overflow = pad->reports.overflow;
			if (overflow) {
				memset(&pad->reports, 0, sizeof(pad->reports));
				pad->usb_ps_suppressed = 1;
			}
			len = npp_report_pop(&pad->reports, buf);
			unlock();
			if (overflow) {
				npp_boot_mark("usb_queue_overflow_release_PS_to_rearm");
				pad->state.buttons &= ~NPP_B_PS;
				ldd_inject(pad);
			}
			if (!len)
				break;

			if (pad->model == NPP_MODEL_DUALSENSE)
				result = dualsense_parse(buf, len, NPP_LINK_USB, &pad->state);
			else
				result = ds4_parse(buf, len, NPP_LINK_USB, &pad->state);
			if (result != 0)
				continue;
			if (!pad->usb_report_seen) {
				pad->usb_report_seen = 1;
				npp_boot_mark("usb_HID_report_valid");
			}
			if (pad->usb_ps_suppressed) {
				if (!(pad->state.buttons & NPP_B_PS))
					pad->usb_ps_suppressed = 0;
				pad->state.buttons &= ~NPP_B_PS;
			}

			static uint32_t s_last_buttons = 0;
			if (pad->state.buttons != s_last_buttons) {
				s_last_buttons = pad->state.buttons;
				npp_boot_mark_hex("BTN_PARSED", s_last_buttons);
				if (len >= 11) {
					npp_boot_mark_hex("RAW_8_9", ((uint32_t)buf[8] << 8) | buf[9]);
					npp_boot_mark_hex("RAW_10_11", ((uint32_t)buf[10] << 8) | buf[11]);
				}
			}

			if (g_cfg.diag)
				diag_state(pad);

			ldd_check_combo(pad);

			/* Only inject when the controller actually sent something.
			 * Feeding libpad on a fixed timer instead - and at 250 Hz -
			 * costs enough system time to stall apps while they load;
			 * the reference plugin inserts on new data only, at 100 Hz. */
			ldd_inject(pad);
		}

		if (pad->out.dirty)
			usb_send_output(pad);
	}
}

/* ------------------------------------------------------------------------ */
/* Extra device ids from the config directory                                */
/* ------------------------------------------------------------------------ */

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static const char *parse_hex(const char *p, uint16_t *out)
{
	uint32_t v = 0;
	int d;

	while (*p == ' ' || *p == '\t') p++;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

	while ((d = hexval(*p)) >= 0) {
		v = (v << 4) | (uint32_t)d;
		p++;
	}

	*out = (uint16_t)v;
	return p;
}

/* Format, one device per line:  0x054c, 0x0ce6, DUALSENSE  */
static void load_extra_devices(void)
{
	static char text[2048];
	char *line, *p;
	int n;

	n = npp_read_file(npp_path(NPP_PATH_DEVICES), text, sizeof(text) - 1);
	if (n <= 0)
		return;
	text[n] = 0;

	line = text;
	while (*line && device_count < MAX_DEVICE_ENTRIES) {
		char *next = line;
		uint16_t vid = 0, pid = 0;

		while (*next && *next != '\n') next++;
		if (*next == '\n') *next++ = 0;

		p = line;
		line = next;

		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == 0)
			continue;

		p = (char *)parse_hex(p, &vid);
		while (*p == ' ' || *p == ',' || *p == '\t') p++;
		p = (char *)parse_hex(p, &pid);
		if (!vid || !pid || lookup(vid, pid))
			continue;

		while (*p == ' ' || *p == ',' || *p == '\t') p++;

		devices[device_count].vid   = vid;
		devices[device_count].pid   = pid;
		devices[device_count].model = (*p == 'D' && p[1] == 'U')
		                              ? NPP_MODEL_DUALSENSE : NPP_MODEL_DS4;
		devices[device_count].name  = "NativePadPS3 device";
		device_count++;
	}
}

int usb_init(void)
{
	sys_mutex_attribute_t attr;
	int i, r, reg_count = 0;

	sys_mutex_attribute_initialize(attr);
	if (sys_mutex_create(&usb_mutex, &attr) != CELL_OK)
		return -1;
	usb_ready = 1;

	load_extra_devices();

	for (i = 0; i < device_count; i++) {
		npp_ops[i].name   = op_names[i];
		npp_ops[i].probe  = npp_probe;
		npp_ops[i].attach = npp_attach;
		npp_ops[i].detach = npp_detach;
		r = cellUsbdRegisterExtraLdd(&npp_ops[i], devices[i].vid, devices[i].pid);
		if (r != CELL_OK) {
			npp_boot_mark_hex("usb_reg_FAIL", (uint32_t)r);
		} else {
			reg_count++;
			npp_boot_mark_hex("usb_reg_OK_pid", (uint32_t)devices[i].pid);
		}
	}

	if (reg_count == 0) {
		usb_shutdown();
		return -1;
	}

	return 0;
}

void usb_shutdown(void)
{
	int i;

	for (i = 0; i < device_count; i++) {
		if (npp_ops[i].name)
			cellUsbdUnregisterExtraLdd(&npp_ops[i]);
	}

	for (i = 0; i < NPP_MAX_PADS; i++) {
		if (g_pads[i].in_use && g_pads[i].link == NPP_LINK_USB) {
			g_pads[i].in_use = 0;
			ldd_unregister(&g_pads[i]);
		}
	}

	if (usb_ready) {
		usb_ready = 0;
		sys_mutex_destroy(usb_mutex);
	}
}
