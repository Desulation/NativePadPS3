/*
 * DS45PAD kernel shim - blob entry points (kernel-ABI glue).
 *
 * This is the one translation unit unique to the blob (shim.c, ds3_report.c,
 * ds3_output.c, src/sixaxis.c and src/ds_body.c are shared with the host tests). It defines
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

/* Bluetooth transport input read (method 4, slot 0x37CE38).
 *
 * Bounce path, EVERY read in modes 1 and 2 (the modes that raise the read limit
 * to 0x50, so a DualSense's full 0x31 report arrives whole), when the shim is
 * enabled and xport and dst are non-NULL, whichever pad it is. The transport
 * read copies the whole packet into its destination with no bound, the limit is
 * shared by every device on the channel and the pad driver's buffer is 64
 * bytes, so no read may go to the caller's buffer directly (and which pad it is
 * cannot be decided before the read: the transport has no pointer back to its
 * pad, and a lookup that misses must not mean an overrun). The original is given
 * the shim's own 96-byte buffer (d->inbuf) and 0x50 as its length instead (it
 * only tests the length for non-zero; the pad manager lock serialises reads, so
 * one buffer is enough). After the read the pad is looked up and
 * shim_input_bounce translates a DualSense's report in the bounce buffer, passes
 * any other pad's bytes through unchanged, and copies at most len_in bytes to the
 * caller's buffer (anything longer is dropped).
 *
 * Normal path - every other case (mode 0 or any other mode value, a disabled
 * shim, NULL arguments): run the original first with the caller's own dst and
 * length to fill dst and get the byte count. Then find the pad that owns this transport in the pad manager
 * and, for an enabled DualSense, let the filter rewrite its report in dst into a
 * DS3 report. An error or implausible count from the original, or a buffer too
 * small for a DS3 report, returns the original result untouched. Every such
 * read - a DS3's included - pays the bounded 16-entry manager scan and is
 * counted in stat_passed by the filter (vid/pid 0 for a non-DualSense), so on
 * the console stat_passed rises with any pad's reads, not just a DualSense's. */
int SHIM_HOOK(shim_input)(void *xport, uint8_t *dst, uint16_t len_in, int flag)
{
	orig_input_fn orig = (orig_input_fn)(uintptr_t)ds45_shim_data.orig_input;
	const uint8_t *pad;
	int rc, ds;
	uint16_t n;

	if (ds45_shim_data.enabled && xport && dst &&
	    (ds45_shim_data.out_mode == 1 || ds45_shim_data.out_mode == 2)) {
		rc = orig(xport, ds45_shim_data.inbuf, SHIM_IN_BOUNCE_LEN, flag);
		pad = rc >= 0 ? shim_pad_from_xport(&ds45_shim_data, xport) : 0;
		return shim_input_bounce(&ds45_shim_data, pad, dst, len_in, rc);
	}

	rc = orig(xport, dst, len_in, flag);
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

/* DS3-route output decision, called by the shim_output stub below, not by the
 * kernel: the stub is entered by a plain `bl` at the kernel's output call (the
 * Bluetooth sender 0x10283C is called with r3 = connection handle, r4 = 2,
 * r5 = report id, r6 = payload, r7 = length), saves those arguments and passes
 * this body the payload (buf), its length and three out-parameter slots in its own
 * frame. A 1 means "replace report id, payload and length with *rid, *obuf,
 * *olen"; the body (shim_output_filter) answers 1 only for a DualSense in the
 * rewriting stages (out_mode 1 and 2) and 0 in observe mode (out_mode 0, which is
 * also what the plugin writes without the input hook). The original sender is
 * tail-called by the stub, never from here. */
int SHIM_HOOK(shim_output)(const uint8_t *buf, uint16_t len, uint8_t *rid,
                           const uint8_t **obuf, uint16_t *olen)
{
	return shim_output_filter(&ds45_shim_data, buf, len, rid, obuf, olen);
}

/* DS3-route power-off decision, called by the shim_poweroff stub below, not by the
 * kernel: the stub is entered by a plain `bl` at the kernel's close-connection
 * send (0x102750 on DEX 4.92, called as (handle, type, report id, payload, length,
 * cookie, object)), stores r3..r9 in a ds45_po_call_t in its own frame and passes
 * its address. A 1 means "replace report id, payload and length with c->o_rid,
 * c->o_buf, c->o_len" (a DualSense's close request in po_mode 1); 0 leaves the
 * call alone. The original sender is tail-called by the stub, never from here. */
int SHIM_HOOK(shim_poweroff)(ds45_po_call_t *c)
{
	return shim_poweroff_filter(&ds45_shim_data, c);
}

/* Pad teardown observation, called by the shim_teardown stub below: the stub is
 * entered at the kernel's pad teardown routine 0x1174FC(address, flag) and passes
 * its two arguments and its link register (the caller's return address) on. It
 * only notes them; the stub tail-calls the original with every register as it
 * came in. */
void SHIM_HOOK(shim_teardown)(uint64_t addr, uint64_t flag, uint64_t lr)
{
	shim_teardown_note(&ds45_shim_data, addr, flag, lr);
}

/* Stub frame slots (shim.h has the frame layout): the numbers the assembly below
 * uses, checked against the call record and the frame constants in both the blob
 * and the host build, so a layout change cannot silently desynchronise the stubs
 * from ds45_po_call_t. */
#define SHIM_STATIC_ASSERT(n, c) typedef char shim_assert_##n[(c) ? 1 : -1]
#define PO_S_HANDLE  0x70
#define PO_S_TYPE    0x78
#define PO_S_RID     0x80
#define PO_S_PAYLOAD 0x88
#define PO_S_LEN     0x90
#define PO_S_COOKIE  0x98
#define PO_S_OBJ     0xA0
#define PO_S_ORID    0xA8
#define PO_S_OBUF    0xB0
#define PO_S_OLEN    0xB8
SHIM_STATIC_ASSERT(po_handle,  SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, handle)  == PO_S_HANDLE);
SHIM_STATIC_ASSERT(po_type,    SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, type)    == PO_S_TYPE);
SHIM_STATIC_ASSERT(po_rid,     SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, rid)     == PO_S_RID);
SHIM_STATIC_ASSERT(po_payload, SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, payload) == PO_S_PAYLOAD);
SHIM_STATIC_ASSERT(po_len,     SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, len)     == PO_S_LEN);
SHIM_STATIC_ASSERT(po_cookie,  SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, cookie)  == PO_S_COOKIE);
SHIM_STATIC_ASSERT(po_obj,     SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, obj)     == PO_S_OBJ);
SHIM_STATIC_ASSERT(po_o_rid,   SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, o_rid)   == PO_S_ORID);
SHIM_STATIC_ASSERT(po_o_buf,   SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, o_buf)   == PO_S_OBUF);
SHIM_STATIC_ASSERT(po_o_len,   SHIM_PO_CALL + __builtin_offsetof(ds45_po_call_t, o_len)   == PO_S_OLEN);
SHIM_STATIC_ASSERT(po_call_fills_frame,
                   SHIM_PO_CALL + sizeof(ds45_po_call_t) == SHIM_PO_SAVED_LR &&
                   SHIM_PO_SAVED_LR + 8 == SHIM_PO_SAVED_R2 &&
                   SHIM_PO_SAVED_R2 + 8 == SHIM_PO_FRAME &&
                   SHIM_PO_CALL == SHIM_STUB_PARAM_HI && SHIM_PO_FRAME % 16 == 0);
#define TD_S_R3  0x80
#define TD_S_R4  0x88
#define TD_S_R5  0x90
#define TD_S_R6  0x98
#define TD_S_R7  0xA0
#define TD_S_R8  0xA8
#define TD_S_R9  0xB0
#define TD_S_R10 0xB8
SHIM_STATIC_ASSERT(td_args_contiguous,
                   TD_S_R3 == SHIM_TD_ARGS && TD_S_R4 == TD_S_R3 + 8 && TD_S_R5 == TD_S_R4 + 8 &&
                   TD_S_R6 == TD_S_R5 + 8 && TD_S_R7 == TD_S_R6 + 8 && TD_S_R8 == TD_S_R7 + 8 &&
                   TD_S_R9 == TD_S_R8 + 8 && TD_S_R10 == TD_S_R9 + 8);
SHIM_STATIC_ASSERT(td_frame,
                   SHIM_TD_SAVED_LR == SHIM_STUB_PARAM_HI && SHIM_TD_SAVED_R2 == SHIM_TD_SAVED_LR + 8 &&
                   SHIM_TD_ARGS == SHIM_TD_SAVED_R2 + 8 && TD_S_R10 + 8 == SHIM_TD_FRAME &&
                   SHIM_TD_FRAME % 16 == 0);

#ifndef DS45_HOST_TEST
/* orig_output's offset in the data block, spelled out for the stub below (an
 * asm block cannot take offsetof). The typedef fails the build if the ABI
 * header ever moves the field. */
#define SHIM_OFF_ORIG_OUTPUT 360   /* 328 until version 12 grew pid_orig by 32 bytes (304 before version 10) */
typedef char shim_off_orig_output_matches_abi[
	(__builtin_offsetof(ds45_shim_data_t, orig_output) == SHIM_OFF_ORIG_OUTPUT) ? 1 : -1];
/* The same for the two power-off hooks' originals (version 12). */
#define SHIM_OFF_ORIG_POWEROFF 984
#define SHIM_OFF_ORIG_TEARDOWN 992
typedef char shim_off_orig_poweroff_matches_abi[
	(__builtin_offsetof(ds45_shim_data_t, orig_poweroff) == SHIM_OFF_ORIG_POWEROFF) ? 1 : -1];
typedef char shim_off_orig_teardown_matches_abi[
	(__builtin_offsetof(ds45_shim_data_t, orig_teardown) == SHIM_OFF_ORIG_TEARDOWN) ? 1 : -1];
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
"\t.globl shim_poweroff\n" "shim_poweroff:\n" "\t.quad .Ltr_poweroff, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_teardown\n" "shim_teardown:\n" "\t.quad .Ltr_teardown, .TOC.@tocbase, .TOC.@tocbase\n"
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

/* shim_poweroff: the close-connection hook, entered by a PLAIN `bl` from the
 * kernel's code at 0xCA9BC (DEX 4.92; gate note G2), replacing the call
 * `bl 0x102750` (the Bluetooth close/send routine, kernel code address stored in
 * ds45_shim_data.orig_poweroff). Like shim_output it has a descriptor only so
 * tools/shim_pack.py finds its code entry (DS45_SHIM_OFF_POWEROFF); nothing
 * dispatches through it, r11 is NOT the blob TOC and r2 is the KERNEL TOC.
 *
 * On entry: r2 kernel TOC, r3 connection handle, r4 type, r5 report id, r6 payload
 * pointer, r7 length, r8 cookie, r9 the request object (r10 is not an argument);
 * LR = 0xCA9C0, the instruction after the kernel's bl; r1 the kernel caller's frame;
 * r14..r31 live. On exit it BRANCHES (bctr) to the original, which must see exactly
 * what the kernel's bl would have given it: r2 = kernel TOC, LR = 0xCA9C0, r1 = the
 * kernel's frame, r3, r4, r8, r9 unchanged and r5, r6, r7 unchanged - or, when the
 * decision rewrote the call, replaced by o_rid, o_buf, o_len. The original then
 * returns straight to the kernel. Written: r0, r2 (restored), r3..r9 (restored,
 * r5..r7 possibly replaced), r10 (by the C body), r11, r12, CTR, cr0 and cr7, LR
 * (restored); gate note G2: the original reads none of r0, r10, r11, r12, CR or CTR
 * before writing them. The C body keeps r14..r31 intact by itself. The frame is
 * below r1: nothing above it is touched.
 *
 * Steps: 1. save LR, r2 and r3..r9 in a new frame (r3..r9 straight into the
 * ds45_po_call_t at SHIM_PO_CALL); 2. adopt the BLOB TOC into r2 exactly as
 * shim_output does (`bcl 20,31` + mflr, the 8-byte literal beside the stub, an
 * R_PPC64_TOC word the packer rebases); 3. call shim_poweroff_impl(&call) (the nop is
 * the TOC-restore slot a linker may rewrite; r2 stays the blob TOC); 4. with r2
 * still the blob TOC, load ds45_shim_data.orig_poweroff (TOC-relative) into CTR;
 * 5. restore the kernel r2, r3, r4, r8, r9, then r5..r7 from o_rid, o_buf, o_len if
 * the result was non-zero, else from rid, payload, len; 6. restore LR, pop the
 * frame, bctr.
 *
 * Frame (SHIM_PO_FRAME = 0xD0 bytes, 16-aligned; the stdu keeps the back chain):
 *   0x00 back chain   0x08 CR   0x10 LR slot (the callee's)   0x18..0x2F reserved
 *   0x30..0x6F parameter save area for the callee (8 doublewords, ELFv1)
 *   0x70 handle  0x78 type  0x80 rid  0x88 payload  0x90 len  0x98 cookie  0xA0 obj
 *   0xA8 o_rid   0xB0 o_buf 0xB8 o_len (the ds45_po_call_t, 0x70..0xBF)
 *   0xC0 saved LR   0xC8 saved r2 */
__asm__(
"\t.text\n"
"\t.p2align 2\n"
".Ltr_poweroff:\n"
"\tmflr 0\n"
"\tstdu 1, -" SHIM_XSTR(SHIM_PO_FRAME) "(1)\n"
"\tstd 0, " SHIM_XSTR(SHIM_PO_SAVED_LR) "(1)\n"
"\tstd 2, " SHIM_XSTR(SHIM_PO_SAVED_R2) "(1)\n"
"\tstd 3, " SHIM_XSTR(PO_S_HANDLE) "(1)\n"
"\tstd 4, " SHIM_XSTR(PO_S_TYPE) "(1)\n"
"\tstd 5, " SHIM_XSTR(PO_S_RID) "(1)\n"
"\tstd 6, " SHIM_XSTR(PO_S_PAYLOAD) "(1)\n"
"\tstd 7, " SHIM_XSTR(PO_S_LEN) "(1)\n"
"\tstd 8, " SHIM_XSTR(PO_S_COOKIE) "(1)\n"
"\tstd 9, " SHIM_XSTR(PO_S_OBJ) "(1)\n"
"\tbcl 20, 31, .Ltr_poweroff_pc\n"
".Ltr_poweroff_pc:\n"
"\tmflr 12\n"
"\tld 2, .Ltr_poweroff_toc-.Ltr_poweroff_pc(12)\n"
"\taddi 3, 1, " SHIM_XSTR(PO_S_HANDLE) "\n"
"\tbl shim_poweroff_impl\n" "\tnop\n"
"\taddis 12, 2, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_POWEROFF) "@toc@ha\n"
"\tld 12, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_POWEROFF) "@toc@l(12)\n"
"\tmtctr 12\n"
"\tcmpwi 7, 3, 0\n"
"\tld 2, " SHIM_XSTR(SHIM_PO_SAVED_R2) "(1)\n"
"\tld 3, " SHIM_XSTR(PO_S_HANDLE) "(1)\n"
"\tld 4, " SHIM_XSTR(PO_S_TYPE) "(1)\n"
"\tld 8, " SHIM_XSTR(PO_S_COOKIE) "(1)\n"
"\tld 9, " SHIM_XSTR(PO_S_OBJ) "(1)\n"
"\tbeq 7, .Ltr_poweroff_keep\n"
"\tld 5, " SHIM_XSTR(PO_S_ORID) "(1)\n"
"\tld 6, " SHIM_XSTR(PO_S_OBUF) "(1)\n"
"\tld 7, " SHIM_XSTR(PO_S_OLEN) "(1)\n"
"\tb .Ltr_poweroff_done\n"
".Ltr_poweroff_keep:\n"
"\tld 5, " SHIM_XSTR(PO_S_RID) "(1)\n"
"\tld 6, " SHIM_XSTR(PO_S_PAYLOAD) "(1)\n"
"\tld 7, " SHIM_XSTR(PO_S_LEN) "(1)\n"
".Ltr_poweroff_done:\n"
"\tld 0, " SHIM_XSTR(SHIM_PO_SAVED_LR) "(1)\n"
"\tmtlr 0\n"
"\taddi 1, 1, " SHIM_XSTR(SHIM_PO_FRAME) "\n"
"\tbctr\n"
"\t.p2align 3\n"
".Ltr_poweroff_toc:\n"
"\t.quad .TOC.@tocbase\n"
);

/* shim_teardown: the pad teardown hook. It is entered at the kernel's teardown
 * routine 0x1174FC(r3 = the 48-bit Bluetooth address as a VALUE, r4 = flag) by `bl`
 * from 0xF4D94 and 0xFDB44 and by a plain tail `b` from 0xF722C (so LR is then the
 * return address of the caller's caller; gate note G2/G6). It observes and changes
 * nothing: it calls shim_teardown_impl(r3, r4, LR) and then branches (bctr) to the
 * original with r1, r2, r3..r10 and LR exactly as they came in. Written: r0, r2
 * (restored), r3..r10 (restored), r11, r12, CTR, LR (restored), and any CR field
 * (the C body may write them all); the original reads none of r0, r11, r12, CR or
 * CTR before writing them (G2: 0x1174FC reads no CR before writing). Same TOC
 * technique as the stubs above (the literal is the packer's R_PPC64_TOC word).
 *
 * Frame (SHIM_TD_FRAME = 0xC0 bytes, 16-aligned; back chain kept by the stdu):
 *   0x00 back chain   0x08 CR   0x10 LR slot (the callee's)   0x18..0x2F reserved
 *   0x30..0x6F parameter save area for the callee (8 doublewords)
 *   0x70 saved LR   0x78 saved r2   0x80..0xBF r3..r10 (one doubleword each)
 * LR is copied to r5 (third argument) before the `bcl` overwrites it. */
__asm__(
"\t.text\n"
"\t.p2align 2\n"
".Ltr_teardown:\n"
"\tmflr 0\n"
"\tstdu 1, -" SHIM_XSTR(SHIM_TD_FRAME) "(1)\n"
"\tstd 0, " SHIM_XSTR(SHIM_TD_SAVED_LR) "(1)\n"
"\tstd 2, " SHIM_XSTR(SHIM_TD_SAVED_R2) "(1)\n"
"\tstd 3, " SHIM_XSTR(TD_S_R3) "(1)\n"
"\tstd 4, " SHIM_XSTR(TD_S_R4) "(1)\n"
"\tstd 5, " SHIM_XSTR(TD_S_R5) "(1)\n"
"\tstd 6, " SHIM_XSTR(TD_S_R6) "(1)\n"
"\tstd 7, " SHIM_XSTR(TD_S_R7) "(1)\n"
"\tstd 8, " SHIM_XSTR(TD_S_R8) "(1)\n"
"\tstd 9, " SHIM_XSTR(TD_S_R9) "(1)\n"
"\tstd 10, " SHIM_XSTR(TD_S_R10) "(1)\n"
"\tmr 5, 0\n"
"\tbcl 20, 31, .Ltr_teardown_pc\n"
".Ltr_teardown_pc:\n"
"\tmflr 12\n"
"\tld 2, .Ltr_teardown_toc-.Ltr_teardown_pc(12)\n"
"\tbl shim_teardown_impl\n" "\tnop\n"
"\taddis 12, 2, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_TEARDOWN) "@toc@ha\n"
"\tld 12, ds45_shim_data+" SHIM_XSTR(SHIM_OFF_ORIG_TEARDOWN) "@toc@l(12)\n"
"\tmtctr 12\n"
"\tld 2, " SHIM_XSTR(SHIM_TD_SAVED_R2) "(1)\n"
"\tld 3, " SHIM_XSTR(TD_S_R3) "(1)\n"
"\tld 4, " SHIM_XSTR(TD_S_R4) "(1)\n"
"\tld 5, " SHIM_XSTR(TD_S_R5) "(1)\n"
"\tld 6, " SHIM_XSTR(TD_S_R6) "(1)\n"
"\tld 7, " SHIM_XSTR(TD_S_R7) "(1)\n"
"\tld 8, " SHIM_XSTR(TD_S_R8) "(1)\n"
"\tld 9, " SHIM_XSTR(TD_S_R9) "(1)\n"
"\tld 10, " SHIM_XSTR(TD_S_R10) "(1)\n"
"\tld 0, " SHIM_XSTR(SHIM_TD_SAVED_LR) "(1)\n"
"\tmtlr 0\n"
"\taddi 1, 1, " SHIM_XSTR(SHIM_TD_FRAME) "\n"
"\tbctr\n"
"\t.p2align 3\n"
".Ltr_teardown_toc:\n"
"\t.quad .TOC.@tocbase\n"
);
#endif
