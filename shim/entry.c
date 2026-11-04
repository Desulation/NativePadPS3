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

/* DS3-route output decision (observe mode), called by the shim_output stub
 * below, not by the kernel: the stub is entered by a plain `bl` at the kernel's
 * output call (the Bluetooth sender 0x10283C is called with r3 = connection
 * handle, r4 = 2, r5 = report id, r6 = payload, r7 = length), saves those
 * arguments and passes this body the payload (buf), its length and three
 * out-parameter slots in its own frame. A 1 means "replace report id, payload
 * and length with *rid, *obuf, *olen"; observe mode always answers 0. The
 * original sender is tail-called by the stub, never from here. */
int SHIM_HOOK(shim_output)(const uint8_t *buf, uint16_t len, uint8_t *rid,
                           const uint8_t **obuf, uint16_t *olen)
{
	return shim_output_filter(&ds45_shim_data, buf, len, rid, obuf, olen);
}

#ifndef DS45_HOST_TEST
/* orig_output's offset in the data block, spelled out for the stub below (an
 * asm block cannot take offsetof). The typedef fails the build if the ABI
 * header ever moves the field. */
#define SHIM_OFF_ORIG_OUTPUT 304
typedef char shim_off_orig_output_matches_abi[
	(__builtin_offsetof(ds45_shim_data_t, orig_output) == SHIM_OFF_ORIG_OUTPUT) ? 1 : -1];
#define SHIM_XSTR_(x) #x
#define SHIM_XSTR(x) SHIM_XSTR_(x)

/* The kernel's vtable dispatch (framework 0xCB5C0, input caller 0x2A4E00) loads
 * the descriptor's env word (opd+0x10) into r11 and the entry (opd+0) but never
 * r2, assuming the one kernel TOC. Our descriptors put the blob TOC in BOTH toc
 * and env, so r11 = blob TOC on entry.
 *
 * The kernel also does NOT reload r2 after the call returns (it assumes every
 * callee preserves the one shared TOC), so each hook is a small framed wrapper:
 * save the kernel's r2 in its own frame, adopt r11 into r2 for the C body's
 * TOC-relative data access, call the impl, restore the kernel's r2 and return.
 * r3..r6 are untouched until the call, so the impl gets its arguments, and the
 * impl's return value (r3) survives the epilogue.
 *
 * "bl shim_x_impl" names the impl's .opd DESCRIPTOR on ELFv1 (the C symbol is
 * the descriptor, not the code); tools/shim_pack.py retargets every such REL24
 * to the descriptor's code entry and fails the build if any is left. The nop
 * after each bl is the slot a linker may rewrite for a TOC restore; we restore
 * r2 ourselves from the frame, right after it. The packer rebases all three
 * descriptor words (entry = ADDR64, toc and env = TOC base) to the load base. */
__asm__(
"\t.section .opd,\"aw\",@progbits\n"
"\t.p2align 3\n"
"\t.globl shim_event\n"  "shim_event:\n"  "\t.quad .Ltr_event, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_enable\n" "shim_enable:\n" "\t.quad .Ltr_enable, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_input\n"  "shim_input:\n"  "\t.quad .Ltr_input, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_output\n" "shim_output:\n" "\t.quad .Ltr_output, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.text\n"
"\t.p2align 2\n"
".Ltr_event:\n"
"\tmflr 0\n" "\tstd 0, 16(1)\n" "\tstdu 1, -0x30(1)\n" "\tstd 2, 0x20(1)\n"
"\tmr 2, 11\n" "\tbl shim_event_impl\n" "\tnop\n"
"\tld 2, 0x20(1)\n" "\taddi 1, 1, 0x30\n" "\tld 0, 16(1)\n" "\tmtlr 0\n" "\tblr\n"
".Ltr_enable:\n"
"\tmflr 0\n" "\tstd 0, 16(1)\n" "\tstdu 1, -0x30(1)\n" "\tstd 2, 0x20(1)\n"
"\tmr 2, 11\n" "\tbl shim_enable_impl\n" "\tnop\n"
"\tld 2, 0x20(1)\n" "\taddi 1, 1, 0x30\n" "\tld 0, 16(1)\n" "\tmtlr 0\n" "\tblr\n"
".Ltr_input:\n"
"\tmflr 0\n" "\tstd 0, 16(1)\n" "\tstdu 1, -0x30(1)\n" "\tstd 2, 0x20(1)\n"
"\tmr 2, 11\n" "\tbl shim_input_impl\n" "\tnop\n"
"\tld 2, 0x20(1)\n" "\taddi 1, 1, 0x30\n" "\tld 0, 16(1)\n" "\tmtlr 0\n" "\tblr\n"
);

/* shim_output: the output hook, entered by a PLAIN `bl` from the kernel's code
 * at the call to the Bluetooth sender 0x10283C (0x2A2DE8 on DEX 4.92; facts note
 * F6), unlike the three hooks above, which the kernel reaches through a vtable
 * descriptor. It has a descriptor only so tools/shim_pack.py finds its code
 * entry (DS45_SHIM_OFF_OUTPUT); nothing dispatches through it, so r11 is NOT the
 * blob TOC here and r2 is the KERNEL TOC.
 *
 * On entry (kernel contract):
 *   r2  kernel TOC             r3  connection handle byte   r4  2 (output)
 *   r5  report id (1)          r6  payload (pad + 0x22B)    r7  length (48)
 *   LR  the instruction after the kernel's bl     r1  the kernel caller's frame
 *   r14..r31 live (the kernel needs them preserved)   r0, r8..r12, cr7 dead.
 * On exit it BRANCHES (bctr) to the original sender, so that must see exactly
 * what the kernel's bl would have given it: r2 = kernel TOC, LR = the kernel's
 * return address, r1 = the kernel's frame, r3, r4 unchanged, and r5, r6, r7
 * unchanged - or, if the decision rewrote the call, replaced by the report id,
 * payload pointer and length (a 16-bit length, in r7). The original then returns
 * straight to the kernel. Registers touched: r0, r2 (restored), r3..r7, r11, r12,
 * CTR, cr0 and cr7, LR (restored); the C body keeps r14..r31 intact by itself.
 * The frame is below r1: nothing above it is touched.
 *
 * Steps: 1. save LR, r2, r3..r7 in a new frame; 2. adopt the BLOB TOC into r2
 * position-independently: `bcl 20,31` (the PC-capture form of bl-to-next that
 * does not unbalance the return-address predictor) + mflr give the address of
 * .Ltr_output_pc, and the 8-byte literal .Ltr_output_toc beside the stub holds
 * `.TOC.@tocbase`, an R_PPC64_TOC word the packer rebases to the load base like
 * the descriptor words; 3. call shim_output_impl(payload, length, &rid, &obuf,
 * &olen) with the three out-parameters in the frame (the nop after the bl is, as
 * in the wrappers above, the TOC-restore slot a linker may rewrite; r2 stays the
 * blob TOC across the call, which is all this needs); 4. with r2 still the blob
 * TOC, load ds45_shim_data.orig_output (TOC-relative, PC-independent) into CTR;
 * 5. restore the kernel r2, r3, r4, then r5..r7 from the frame, or from the
 * out-parameters if the result was non-zero; 6. restore LR, pop the frame, bctr.
 *
 * Frame (0xC0 bytes, 16-aligned; the stdu keeps the back chain):
 *   0x00 back chain   0x08 CR   0x10 LR slot (the callee's)   0x18..0x2F reserved
 *   0x30..0x6F parameter save area for the callee (8 doublewords, ELFv1)
 *   0x70 rid (byte)   0x78 obuf (pointer)   0x80 olen (halfword)
 *   0x88 saved LR   0x90 saved r2   0x98 r3   0xA0 r4   0xA8 r5   0xB0 r6   0xB8 r7
 * The three out-parameters are read back at the very addresses (and widths) the
 * body stores them, so byte order and zero-extension need no thought. */
__asm__(
"\t.text\n"
"\t.p2align 2\n"
".Ltr_output:\n"
"\tmflr 0\n"
"\tstdu 1, -0xC0(1)\n"
"\tstd 0, 0x88(1)\n"
"\tstd 2, 0x90(1)\n"
"\tstd 3, 0x98(1)\n"
"\tstd 4, 0xA0(1)\n"
"\tstd 5, 0xA8(1)\n"
"\tstd 6, 0xB0(1)\n"
"\tstd 7, 0xB8(1)\n"
"\tbcl 20, 31, .Ltr_output_pc\n"
".Ltr_output_pc:\n"
"\tmflr 12\n"
"\tld 2, .Ltr_output_toc-.Ltr_output_pc(12)\n"
"\tmr 3, 6\n"
"\tclrldi 4, 7, 48\n"
"\taddi 5, 1, 0x70\n"
"\taddi 6, 1, 0x78\n"
"\taddi 7, 1, 0x80\n"
"\tbl shim_output_impl\n" "\tnop\n"
"\taddis 12, 2, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_OUTPUT) "@toc@ha\n"
"\tld 12, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_OUTPUT) "@toc@l(12)\n"
"\tmtctr 12\n"
"\tcmpwi 7, 3, 0\n"
"\tld 2, 0x90(1)\n"
"\tld 3, 0x98(1)\n"
"\tld 4, 0xA0(1)\n"
"\tbeq 7, .Ltr_output_keep\n"
"\tlbz 5, 0x70(1)\n"
"\tld 6, 0x78(1)\n"
"\tlhz 7, 0x80(1)\n"
"\tb .Ltr_output_done\n"
".Ltr_output_keep:\n"
"\tld 5, 0xA8(1)\n"
"\tld 6, 0xB0(1)\n"
"\tld 7, 0xB8(1)\n"
".Ltr_output_done:\n"
"\tld 0, 0x88(1)\n"
"\tmtlr 0\n"
"\taddi 1, 1, 0xC0\n"
"\tbctr\n"
"\t.p2align 3\n"
".Ltr_output_toc:\n"
"\t.quad .TOC.@tocbase\n"
);
#endif
