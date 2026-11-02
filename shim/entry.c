/*
 * DS45PAD kernel shim - blob entry points (kernel-ABI glue).
 *
 * This is the one translation unit unique to the blob (shim.c, ds3_report.c,
 * src/sixaxis.c and src/ds_body.c are shared with the host tests). It defines
 * the single data object the plugin fills in, and the three wrappers the plugin
 * points the kernel's DS3 driver at: the start-up driver's event handler, the
 * enable (0xF4) driver's event handler and the Bluetooth transport's input read.
 * These bodies are thin glue: they hand the hook arguments to the pure, tested
 * decision functions in shim.c, and otherwise tail-call the originals so a
 * DualShock 3 is left completely untouched. They send nothing, take no lock,
 * allocate nothing and call nothing in the kernel but the original.
 *
 * Built freestanding into a flat, position-independent PPC64 blob that runs
 * INSIDE the PS3 kernel, so: no SDK, no libc, no static constructors, and every
 * pointer is 64-bit (the kernel's live addresses are 0x8000_0000_xxxx_xxxx and
 * must not be truncated). See shim/shim.ld and tools/shim_pack.py.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

#include <stdint.h>
#include "shim_abi.h"
#include "shim.h"
#include "ds3_report.h"

/* The one object the plugin and the blob share at run time. The plugin writes
 * the configuration, the original descriptors, the pad class and manager
 * address and the info template into it when it installs the shim, and reads
 * the counters back; the blob reads the rest.
 *
 * It carries its magic and version pre-filled so the installer can sanity-check
 * the image it just relocated, which also keeps the object in .data (a real,
 * file-backed part of the blob image) rather than .bss. Its offset in the image
 * is exported as DS45_SHIM_OFF_DATA. */
ds45_shim_data_t ds45_shim_data = {
	.magic   = DS45_SHIM_MAGIC,
	.version = DS45_SHIM_VERSION,
};

/* On the 64-bit PowerPC ELFv1 ABI a function pointer is the address of a
 * { entry, toc, env } descriptor; calling through one loads the callee's TOC
 * (r2) and entry from it. orig_event / orig_enable / orig_input hold the
 * addresses of the kernel's own descriptors, so a plain indirect call tail-calls
 * the originals with their own r2 restored. */
typedef int (*orig_event_fn)(void *drv, long status, long len);
typedef int (*orig_input_fn)(void *xport, uint8_t *dst, uint16_t len_in,
                             int flag);

/* Start-up driver event handler (slot 0x37CFF8). A DualSense refuses every DS3
 * start-up request; on its first event, whatever the status, the shim fills the
 * pad's DS3 info block and identity, marks the driver done with status 0 and
 * answers 0, so the framework calls the ready callback with status 0. Anything
 * else - another pad, a driver already done, the shim disabled - goes to the
 * original with the same arguments. */
int shim_event(void *drv, long status, long len)
{
	if (shim_event_complete(&ds45_shim_data, (uint8_t *)drv))
		return 0;
	return ((orig_event_fn)(uintptr_t)ds45_shim_data.orig_event)(drv, status, len);
}

/* Enable (0xF4) driver event handler (slot 0x37CFB0): the port is assigned only
 * if this driver completes with status 0. For a DualSense not yet done it
 * stores status 0 and the done flag and answers 0; everything else goes to the
 * original. */
int shim_enable(void *drv, long status, long len)
{
	if (shim_enable_complete(&ds45_shim_data, (uint8_t *)drv))
		return 0;
	return ((orig_event_fn)(uintptr_t)ds45_shim_data.orig_enable)(drv, status, len);
}

/* Bluetooth transport input read (method 4, slot 0x37CE38). Always run the
 * original first to fill dst and get the byte count. Then find the pad that
 * owns this transport in the pad manager (the transport has no pointer back to
 * its pad) and, for an enabled DualSense, let the filter rewrite its report in
 * dst into a DS3 report. An error or implausible count from the original, or a
 * buffer too small for a DS3 report, returns the original result untouched.
 * Every other read - a DS3's included - pays the bounded 16-entry manager scan
 * and is counted in stat_passed by the filter (vid/pid 0 for a non-DualSense),
 * so on the console stat_passed rises with any pad's reads, not just a
 * DualSense's. */
int shim_input(void *xport, uint8_t *dst, uint16_t len_in, int flag)
{
	int rc = ((orig_input_fn)(uintptr_t)ds45_shim_data.orig_input)(xport, dst,
	                                                               len_in, flag);
	const uint8_t *pad;
	int ds;
	uint16_t n;

	if (!ds45_shim_data.enabled || !xport || !dst || rc < 0 || rc > 0x41 ||
	    len_in < DS3_REPORT_LEN)
		return rc;

	/* Find the pad once, note what the hook saw for the debug trace, then decide
	 * the identity from that same pad (no second manager scan). */
	pad = shim_pad_from_xport(&ds45_shim_data, xport);
	shim_input_note(&ds45_shim_data, pad);
	ds = shim_pad_is_dualsense(&ds45_shim_data, pad);
	n = (uint16_t)rc;
	return shim_input_filter(&ds45_shim_data, ds ? 0x054C : 0, ds ? 0x0CE6 : 0,
	                         dst, &n);
}
