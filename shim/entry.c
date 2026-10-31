/*
 * DS45PAD kernel shim - blob entry points (kernel-ABI glue).
 *
 * This is the one translation unit unique to the blob (shim.c, ds3_report.c,
 * src/sixaxis.c and src/ds_body.c are shared with the host tests). It defines
 * the single data object the plugin fills in, and the two wrappers the plugin
 * patches the kernel's DS3 driver to call (the product-ID helper is
 * shim_pid_ok in shim.c, which the kernel branches to directly). These bodies
 * are the thin, untested glue: they pull VID/PID, the buffer and the length out
 * of the real F1/F2 argument lists and the data block, hand them to the pure,
 * tested decision functions in shim.c, and otherwise tail-call the originals so
 * a DualShock 3 is left completely untouched.
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

/* The one object the plugin and the blob share at run time. The plugin writes
 * the configuration, the original descriptors, the transfer-routine descriptor
 * and the per-connection identity and connection id into it (the first three
 * when it installs the shim, the last as connections come up) and reads the
 * counters back; the blob reads the configuration from it.
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
 * (r2) and entry from it. orig_startup / orig_input hold the addresses of the
 * kernel's own descriptors (fact sheet F1/F2 prototypes), so a plain indirect
 * call tail-calls the originals with their own r2 restored. */
typedef int (*orig_startup_fn)(void *drv, long arg1);
typedef int (*orig_input_fn)(void *xport, uint8_t *dst, uint16_t len_in,
                             int flag);

/* The Bluetooth connection handle in each hook's arguments (fact sheet F1/F2):
 * the start-up driver object carries it at drv+0xAC, the input transport object
 * at xport+8. It is the only identity field within reach of the hook args. */
#define F1_HANDLE_OFF   0xAC
#define F2_HANDLE_OFF   0x08

/* Resolve a connection handle to the identity the plugin recorded in the data
 * block. The plugin fills a slot when a controller connects - conn_key[i] holds
 * that connection's handle, conn_id[i] the id the kernel's transfer routine
 * takes for it, and conn_state[i] is non-zero for an enabled DualSense (set
 * last) - resolving VID/PID the confirmed way (the pad object at +0x68) rather
 * than from these hook arguments, which do not carry it. Deriving VID/PID
 * straight from the hook args is the deferred residual U1; the blob only reads
 * the slots the plugin fills. Returns the matching enabled slot's index, with
 * the DualSense identity, else -1 (which makes the caller tail-call the
 * original). The index reaches the rest of that connection's state. */
static int entry_identity(uint8_t handle, uint16_t *vid, uint16_t *pid)
{
	int i;

	for (i = 0; i < DS45_SHIM_CONNS; i++) {
		if (ds45_shim_data.conn_state[i] != 0 &&
		    (uint8_t)ds45_shim_data.conn_key[i] == handle) {
			*vid = 0x054C;
			*pid = 0x0CE6;
			return i;
		}
	}
	return -1;
}

/* F1: Bluetooth Sony-pad start-up. For an enabled DualSense, do its own enable
 * (shim_startup_dualsense, given the connection's slot, sends the feature-0x05
 * GET to the connection id the plugin recorded in it: the hook argument carries
 * only the 1-byte handle, not the connection object, so the id is not derivable
 * here - fact sheet F1, note P3/R3 - and it bounds how often it sends, since
 * the driver may call this again and again); for everything else - disabled,
 * the original's arg1==0 early-return, an unknown connection or a DualShock 3 -
 * tail-call the original so it is untouched. */
int shim_startup(void *drv, long arg1)
{
	uint16_t vid, pid;
	int slot;

	if (ds45_shim_data.enabled && arg1 != 0 && drv) {
		slot = entry_identity(*((const uint8_t *)drv + F1_HANDLE_OFF), &vid, &pid);
		if (slot >= 0 && shim_is_dualsense(vid, pid))
			return shim_startup_dualsense(&ds45_shim_data, slot, arg1);
	}

	return ((orig_startup_fn)(uintptr_t)ds45_shim_data.orig_startup)(drv, arg1);
}

/* F2: Bluetooth transport input read (method 4). Always run the original first
 * to fill dst and get the byte count, then, for an enabled DualSense, let the
 * filter rewrite a 0x31 report in dst into a DS3 report; for anything else, or
 * an error/implausible count from the original, return the original result
 * untouched. */
int shim_input(void *xport, uint8_t *dst, uint16_t len_in, int flag)
{
	int rc = ((orig_input_fn)(uintptr_t)ds45_shim_data.orig_input)(xport, dst,
	                                                               len_in, flag);
	uint16_t vid, pid, n;

	if (!ds45_shim_data.enabled || !xport || !dst || rc < 0 || rc > 0x41)
		return rc;
	if (entry_identity(*((const uint8_t *)xport + F2_HANDLE_OFF), &vid, &pid) < 0)
		return rc;

	n = (uint16_t)rc;
	return shim_input_filter(&ds45_shim_data, vid, pid, dst, &n);
}
