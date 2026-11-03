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

/* Hook naming. The kernel calls the hook through a vtable slot that points at a
 * { entry, toc, env } descriptor. In the blob build the C bodies below are
 * named *_impl and the descriptors/trampolines (asm block at the end of this
 * file) are named shim_event / shim_enable / shim_input; in the host test build
 * the C function itself is the hook, so tests/test_shim.c can call it. */
#ifdef DS45_HOST_TEST
#define SHIM_HOOK(n) n              /* host: the C function IS the hook (tests call it) */
#else
#define SHIM_HOOK(n) n##_impl       /* blob: C body is the impl; asm trampoline is the hook */
#endif

/* Start-up driver event handler (slot 0x37CFF8). A DualSense refuses every DS3
 * start-up request; on its first event, whatever the status, the shim fills the
 * pad's DS3 info block and identity, marks the driver done with status 0 and
 * answers 0, so the framework calls the ready callback with status 0. Anything
 * else - another pad, a driver already done, the shim disabled - goes to the
 * original with the same arguments. */
int SHIM_HOOK(shim_event)(void *drv, long status, long len)
{
	if (shim_event_complete(&ds45_shim_data, (uint8_t *)drv))
		return 0;
	return ((orig_event_fn)(uintptr_t)ds45_shim_data.orig_event)(drv, status, len);
}

/* Enable (0xF4) driver event handler (slot 0x37CFB0): the port is assigned only
 * if this driver completes with status 0. For a DualSense not yet done it
 * stores status 0 and the done flag and answers 0; everything else goes to the
 * original. */
int SHIM_HOOK(shim_enable)(void *drv, long status, long len)
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
int SHIM_HOOK(shim_input)(void *xport, uint8_t *dst, uint16_t len_in, int flag)
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

#ifndef DS45_HOST_TEST
/* The kernel's vtable dispatch (framework 0xCB5C0, input caller 0x2A4E00) loads
 * the descriptor's env word (opd+0x10) into r11 and the entry (opd+0) but never
 * r2, assuming the one kernel TOC. Our descriptors put the blob TOC in BOTH toc
 * and env, so r11 = blob TOC on entry; the trampoline adopts it into r2 before
 * the C body's TOC-relative data access. It then jumps to the impl's code through
 * the impl's own function descriptor (a plain "b shim_x_impl" would branch INTO
 * the .opd descriptor, because on ELFv1 the C symbol names the descriptor, not
 * the code): r11 = word 0 of that descriptor, read TOC-relative so it stays
 * position independent. Only r11 and CTR are clobbered; r3..r6 and LR are kept,
 * so the impl returns straight to the kernel. The packer rebases all three
 * descriptor words (entry = ADDR64, toc and env = TOC base) to the load base. */
__asm__(
"\t.section .opd,\"aw\",@progbits\n"
"\t.p2align 3\n"
"\t.globl shim_event\n"  "shim_event:\n"  "\t.quad .Ltr_event, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_enable\n" "shim_enable:\n" "\t.quad .Ltr_enable, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_input\n"  "shim_input:\n"  "\t.quad .Ltr_input, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.text\n"
"\t.p2align 2\n"
".Ltr_event:\n"  "\tmr 2, 11\n"
"\taddis 11, 2, shim_event_impl@toc@ha\n"
"\tld 11, shim_event_impl@toc@l(11)\n"
"\tmtctr 11\n" "\tbctr\n"
".Ltr_enable:\n" "\tmr 2, 11\n"
"\taddis 11, 2, shim_enable_impl@toc@ha\n"
"\tld 11, shim_enable_impl@toc@l(11)\n"
"\tmtctr 11\n" "\tbctr\n"
".Ltr_input:\n"  "\tmr 2, 11\n"
"\taddis 11, 2, shim_input_impl@toc@ha\n"
"\tld 11, shim_input_impl@toc@l(11)\n"
"\tmtctr 11\n" "\tbctr\n"
);
#endif
