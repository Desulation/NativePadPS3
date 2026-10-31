/*
 * DS45PAD kernel shim - decision logic (start-up, input, product ID).
 *
 * Built twice, exactly like ds3_report.c: natively by the host tests, and
 * freestanding into the shim blob that runs inside the PS3 kernel's DS3 driver.
 * So no SDK, no libc, no static data (nothing that would leave a .rodata/TOC
 * entry the packer cannot relocate). Each function takes the data block and the
 * already-extracted arguments explicitly, so the kernel-specific work (reading
 * VID/PID, the buffer and the length out of the F1/F2 argument lists) stays in
 * the untested glue in entry.c and these bodies stay pure and testable.
 *
 * The interfaces and the conventions used here are shim.h and fact sheet
 * F1-F4. Licensed under GPLv2 - see LICENSE.
 */

#include "shim.h"
#include "ds3_report.h"

/* Sony USB identity. */
#define VID_SONY        0x054C
#define PID_DS3         0x0268   /* DualShock 3            */
#define PID_MOVE_NAV    0x042F   /* PlayStation Move navigation */
#define PID_DUALSENSE   0x0CE6   /* DualSense              */

/* DualSense Bluetooth full report id (the same value as DS_IN_BT; redefined
 * here because the freestanding shim must not pull in ds45_state.h). */
#define DS_BT_INPUT_ID  0x31

int shim_is_dualsense(uint16_t vid, uint16_t pid)
{
	return vid == VID_SONY && pid == PID_DUALSENSE;
}

/* F4 helper. A leaf of immediate comparisons only: no global and no TOC access,
 * so the kernel can branch to it with its own r2 (tools/shim_pack.py enforces
 * this over this function's code range). Keeps 0x0268 and 0x042F, adds 0x0CE6;
 * 0x03D5 is accepted only at the inline SDP site, not by this helper. */
int shim_pid_ok(uint32_t vid, uint32_t pid)
{
	return vid == VID_SONY &&
	       (pid == PID_DS3 || pid == PID_MOVE_NAV || pid == PID_DUALSENSE);
}

int shim_input_filter(ds45_shim_data_t *d, uint16_t vid, uint16_t pid,
                      uint8_t *buf, uint16_t *len)
{
	/* Translate only an enabled DualSense's 0x31 report. ds3_report_from_
	 * dualsense reads the whole report into its own state before it writes a
	 * byte, so buf may be its own output (an in-place rewrite), and on any
	 * refusal - wrong id, too short - it writes nothing and returns -1, which
	 * leaves buf and *len exactly as the original read left them. */
	if (d && d->enabled && buf && len &&
	    shim_is_dualsense(vid, pid) &&
	    buf[0] == DS_BT_INPUT_ID &&
	    ds3_report_from_dualsense(buf, (int)*len, &d->cfg, buf) == 0) {
		*len = DS3_REPORT_LEN;
		d->stat_translated++;
		return DS3_REPORT_LEN;
	}

	if (d)
		d->stat_passed++;
	return len ? (int)*len : 0;
}

int shim_startup_dualsense(ds45_shim_data_t *d, void *drv, long arg1)
{
	shim_get_report_fn get;
	int rc;

	/* arg1 is the F1 dispatch argument; entry.c has already sent the
	 * original's arg1==0 early-return to d->orig_startup, and a DualSense is
	 * brought up by the GET below, not by anything keyed on arg1. */
	(void)arg1;

	if (!d)
		return SHIM_F1_OK;

	get = (shim_get_report_fn)(uintptr_t)d->get_report;
	if (!get)
		return SHIM_F1_ERR;

	/* Kick off the pad's 0x31 report stream by reading its calibration
	 * feature report (F3). The 3->running transition is then produced by the
	 * input wrapper translating those reports, so on success the shim just
	 * returns F1's running condition and lets the driver keep polling. */
	rc = get(drv, SHIM_HID_FEATURE, SHIM_DS_CALIB_REPORT_ID);
	if (rc == SHIM_F1_OK) {
		d->stat_startups++;
		return SHIM_F1_OK;
	}
	return rc;
}
