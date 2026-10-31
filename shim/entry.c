/*
 * DS45PAD kernel shim - blob entry points.
 *
 * This is the one translation unit that is unique to the blob (the other three
 * - ds3_report.c, src/sixaxis.c, src/ds_body.c - are shared with the host
 * tests). It defines the single data object the plugin fills in and the three
 * entry points the plugin patches the kernel's DS3 driver to call.
 *
 * It is built freestanding into a flat, position-independent PPC64 blob that
 * runs INSIDE the PS3 kernel, so: no SDK, no libc, no static constructors, and
 * every pointer is 64-bit (the kernel's live addresses are 0x8000_0000_xxxx_
 * xxxx and must not be truncated). See shim/shim.ld and tools/shim_pack.py.
 *
 * These three bodies are deliberately thin placeholders for the build
 * pipeline: shim_startup / shim_input tail-call the originals the plugin
 * recorded, and shim_pid_ok returns the answer it is handed. The real
 * translation is wired in by a later task; what this task pins down is that the
 * blob links, relocates and packs cleanly.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

#include <stdint.h>
#include "shim_abi.h"

/* The one object the plugin and the blob share at run time. The plugin writes
 * the configuration and the original descriptors into it when it installs the
 * shim and reads the counters back; the blob reads the configuration from it.
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
 * kernel's own descriptors, so a plain indirect call tail-calls the originals
 * with their own r2 restored. */
typedef void (*shim_startup_fn)(void);
typedef int  (*shim_input_fn)(void *ctx, uint8_t *report, int report_len);

/* F1: driver start-up hook. Count the call and hand off to the original. */
void shim_startup(void)
{
	ds45_shim_data.stat_startups++;
	((shim_startup_fn)(uintptr_t)ds45_shim_data.orig_startup)();
}

/* F2: input-report hook. Count the call and hand off to the original. The real
 * shim will translate report[] in place before this point. */
int shim_input(void *ctx, uint8_t *report, int report_len)
{
	ds45_shim_data.stat_passed++;
	return ((shim_input_fn)(uintptr_t)ds45_shim_data.orig_input)(ctx, report,
	                                                             report_len);
}

/*
 * F?: the kernel's "is this process allowed the pad" predicate. The kernel
 * calls this one by its raw code address, with the KERNEL's r2 in place - not
 * the blob's - so it must never touch the TOC or any global (tools/shim_pack.py
 * enforces exactly that). Its image offset is exported as DS45_SHIM_OFF_PID_OK
 * as a code address, not a descriptor. The placeholder is transparent: it
 * returns the original answer it is handed.
 */
int shim_pid_ok(int orig_answer)
{
	return orig_answer;
}
