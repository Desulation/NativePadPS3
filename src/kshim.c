/*
 * DS45PAD - install the DS3-route kernel shim on a fingerprinted kernel.
 * See kshim.h and the start-up completion design spec, section 3.
 *
 * Install order (fixed):
 *   0. the ENABLE_SHIM gate: with g_cfg.enable_shim off (its default) stop here -
 *      nothing is read or written;
 *   1. match a table row by fingerprint;
 *   2. verify the three redirect slots (event, enable, input) and the spare
 *      syscall slot by value, the event and enable handlers by the hash of their
 *      first code qword, and the product-ID sites to be edited by the hash of
 *      their qword - writing nothing if any differs, unless the redirects already
 *      point at one of our blobs (adopt it);
 *   3. take the fixed executable base (base = exec_base; design Amendment 2):
 *      verify the whole region [base, base + blob + flush OPD) currently reads
 *      zero, that it ends at or before exec_bound, and that base is 8-aligned
 *      inside the window - writing nothing if any fails;
 *   4. range-check the branch rows against base (the BRANCH helper row must
 *      reach shim_pid_ok, the BRANCHL output call site the output entry, the
 *      power-off branches their stubs - WORD and PTR rows hold a finished
 *      instruction or pointer, no range);
 *   5. copy the blob qword by qword, rebasing relocated words;
 *   6. fill the data block (originals, pad class and manager, info template,
 *      cfg, enabled, and each product-ID site's original qword with the set of
 *      sites applied - in kernel RAM before the first word edit, so a later
 *      load that adopts the blob can undo the edits);
 *   7. flush the instruction cache over the blob (F5), by borrowing the spare
 *      syscall slot once (its only use here), pointed at { flush entry, TOC };
 *   8. apply, transactionally, the helper word, the inline words (only with
 *      SHIM_INLINE_PID), the output sites (only with SHIM_OUTPUT_HOOK and a row
 *      that names the output sender: the two packet-buffer TOC slots back to
 *      back, the call site, then - in probe and full mode with SHIM_INPUT_HOOK
 *      only - the cap word), the power-off sites (only with SHIM_POWEROFF_HOOK
 *      and a row that names the power-off sender: its call site, then in observe
 *      mode and with a row that names the teardown routine the three teardown
 *      observers), the event redirect, the enable redirect and the input
 *      redirect, then (same condition as the cap word) the three receive-limit
 *      words, in that order. The unload mirrors it, last first.
 *
 * Every write is read back. Any failure restores the borrowed spare slot (when
 * it was borrowed) and leaves every pre-existing kernel structure byte-identical
 * to before (the fixed blob region is scratch and is not part of the pre-install
 * image; kpatch_apply undoes its own partial writes). The start-up driver's send
 * slot (0x37CFE8 on DEX 4.92) is not in the row and is never touched. It is
 * entirely RAM-only.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

#include <stddef.h>
#include <string.h>
#include <sys/process.h>   /* system_call_2 / return_to_user_prog (spare syscall) */

#include "lv2.h"
#include "kpatch.h"
#include "kernel.h"        /* kernel_fingerprint_matches, DS45_FP_LEN */
#include "kshim.h"
#include "ds45pad.h"       /* g_cfg, DS45_TRACE */
#include "../shim/shim_abi.h"  /* ds45_shim_data_t, DS45_SHIM_MAGIC */
#include "shim_blob.h"     /* ds45_shim_blob, ds45_shim_relocs, DS45_SHIM_OFF_* */

/* 16 bytes past the blob for the flush OPD { entry, kernel TOC }. */
#define KSHIM_FLUSH_OPD_LEN 16

/* The final transactional apply: the helper word, the inline words (when
 * enabled), the output and power-off sites (when enabled), then the event,
 * enable and input redirects, then the receive-limit words (when enabled). The
 * journal holds one entry for each: at most 17, within the patcher's 20. */
#define KSHIM_REDIRECTS 3
#define KSHIM_FINAL_MAX (DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + \
                         DS45_SHIM_OUTPUT_PATCHES + DS45_SHIM_RECV_PATCHES + \
                         DS45_SHIM_POWEROFF_PATCHES + KSHIM_REDIRECTS)
typedef char kshim_final_fits_kpatch[(KSHIM_FINAL_MAX <= DS45_KPATCH_MAX) ? 1 : -1];
typedef char kshim_final_is_seventeen[(KSHIM_FINAL_MAX == 17) ? 1 : -1];

/* The data block exactly fills the blob's tail (OFF_DATA .. BLOB_LEN); it is
 * written qword by qword, so its size must be a multiple of 8. */
typedef char kshim_data_multiple_of_8[(sizeof(ds45_shim_data_t) % 8 == 0) ? 1 : -1];

/* The version the adoption check reads is the high word of an aligned qword, and
 * the template fits its array. */
typedef char kshim_version_qword[(offsetof(ds45_shim_data_t, version) % 8 == 0) ? 1 : -1];
typedef char kshim_tmpl_fits[(sizeof(((ds45_shim_data_t *)0)->info_tmpl) >=
                              DS45_SHIM_INFO_LEN) ? 1 : -1];

/* The data block records one original qword per product-ID site a row can
 * edit, and an adopted install journals those sites plus the redirects. */
typedef char kshim_pid_sites_match[(DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES +
                                    DS45_SHIM_OUTPUT_PATCHES + DS45_SHIM_RECV_PATCHES +
                                    DS45_SHIM_POWEROFF_PATCHES ==
                                    DS45_SHIM_PID_SITES) ? 1 : -1];
typedef char kshim_adopt_fits_journal[(DS45_SHIM_PID_SITES + KSHIM_REDIRECTS <=
                                       DS45_KPATCH_MAX) ? 1 : -1];
typedef char kshim_pid_words_aligned[(offsetof(ds45_shim_data_t, pid_orig) % 8 == 0 &&
                                      offsetof(ds45_shim_data_t, pid_applied) % 8 == 0) ? 1 : -1];

/* What a successful (or adopted) install left behind, for unload. g_base and
 * g_zero_size are the fixed blob region [g_base, g_base + g_zero_size) that the
 * scrub zeroes; both are set on a fresh install and on a VALID adoption (an
 * adoption whose base is not the row's exec_base, or does not fit, leaves
 * g_zero_size 0 so it is never scrubbed). Recording the size is cleaner than
 * recomputing it from a journalled slot at unload, when the row - and so
 * exec_base and flush_desc - is no longer in hand. g_scrub_armed is set only by
 * kshim_unhook once it has confirmed the redirects reverted, so a half-reverted
 * unload never zeroes a blob a live redirect still points at. */
static ds45_kjournal_t g_journal;
static kshim_result_t  g_state = KSHIM_OFF;
static uint64_t        g_base;
static uint64_t        g_zero_size;
static int             g_scrub_armed;

/* ------------------------------------------------------------------------ */

/* Issue the borrowed spare syscall. The number is a table field, so it is a
 * runtime value; the SDK macro loads it into r11, which a runtime value is
 * fine for. The result (the flush routine's, or the probe stub's) comes back
 * in r3. */
static uint64_t kshim_spare_call(uint32_t num, uint64_t a1, uint64_t a2)
{
	system_call_2(num, a1, a2);
	return_to_user_prog(uint64_t);
}

static uint64_t be64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;
	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static int off_is_reloc(uint32_t off)
{
	int i;
	for (i = 0; i < DS45_SHIM_RELOC_COUNT; i++)
		if (ds45_shim_relocs[i] == off)
			return 1;
	return 0;
}

/* FNV-1a 64 of a qword's 8 bytes in memory order (the same scheme kpatch uses). */
static uint64_t qword_hash(uint64_t v)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(v >> (56 - 8 * i));
	return ds45_fnv1a64(b, 8);
}

/* The 8-aligned qword containing addr (addr & ~7) hashes to expect; 0 if it
 * differs or cannot be read. A row's code hash for an unaligned address (the
 * enable handler's 0xCB234) is therefore the hash of the qword at 0xCB230, never
 * of the 8 bytes starting at 0xCB234 (kshim.h). */
static int hash_ok(uint64_t addr, uint64_t expect)
{
	uint64_t cur;

	if (ds45_lv2_read(addr & ~7ULL, &cur))
		return 0;
	return qword_hash(cur) == expect;
}

/* The output sites by index (the data-block site numbers, shim_abi.h). */
#define SITE_OUT_CALL 3   /* the bl to the output sender, retargeted at the blob's entry */
#define SITE_OUT_CAP  4   /* the send-length cap compare */
#define SITE_BUF      5   /* TOC slot holding the packet buffer's address */
#define SITE_BUF1     6   /* TOC slot holding that address plus one */
#define SITE_RECV0    7   /* the receive-limit words are sites 7, 8 and 9 */
#define SITE_PO0      10  /* the power-off patches are sites 10-13 (above the receive words) */

/* Whether site i is one of the receive-limit words (7-9). NOT `i >= SITE_RECV0`:
 * the power-off sites 10-13 sit above the receive sites. */
static int site_is_recv(int i)
{
	return i >= SITE_RECV0 && i < SITE_PO0;
}

/* Product-ID site i of a row, in data-block order: the helper rows, then the
 * inline rows, then the output rows (sites 3-6 = output_patches[0..3]), then the
 * receive-limit rows (sites 7-9 = recv_patches[0..2]), then the power-off rows
 * (sites 10-13 = poweroff_patches[0..3]). */
static const ds45_kpatch_t *pid_site(const ds45_shim_kernel_t *k, int i)
{
	if (i < DS45_SHIM_PID_PATCHES)
		return &k->pid_patches[i];
	if (i < DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES)
		return &k->inline_patches[i - DS45_SHIM_PID_PATCHES];
	if (i < DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + DS45_SHIM_OUTPUT_PATCHES)
		return &k->output_patches[i - DS45_SHIM_PID_PATCHES - DS45_SHIM_INLINE_PATCHES];
	if (i < SITE_PO0)
		return &k->recv_patches[i - DS45_SHIM_PID_PATCHES - DS45_SHIM_INLINE_PATCHES -
		                        DS45_SHIM_OUTPUT_PATCHES];
	return &k->poweroff_patches[i - SITE_PO0];
}

/* The order the sites are CAPTURED in (data-block order stays site order): the
 * helper and inline rows as before, then the two buffer slots back to back, then
 * the call site, then the cap word, then the four power-off sites (10-13), then the
 * three receive-limit words. The transaction itself applies the sites 0..6 and
 * 10..13 in this order, then the redirects, and only THEN the three receive-limit
 * words (kshim_install_from sets them aside and appends them last; adoption
 * journals them in the same place). So the buffer is in the blob before anything
 * can send into it, the longer cap and the redirects
 * (the input bounce included) are live before the larger read limits go in, and the
 * unload - which undoes the journal last first - lowers the limits back to 0x41
 * FIRST, while the bounce is still in place, and only then takes the bounce away.
 * (The other order leaves a window in which the original transport read, with the
 * limit still 0x50, copies a 79-byte DualSense report into the pad driver's
 * 64-byte stack buffer.) */
static const uint8_t PID_ORDER[DS45_SHIM_PID_SITES] = {
	0, 1, 2, SITE_BUF, SITE_BUF1, SITE_OUT_CALL, SITE_OUT_CAP,
	SITE_PO0, SITE_PO0 + 1, SITE_PO0 + 2, SITE_PO0 + 3,
	SITE_RECV0, SITE_RECV0 + 1, SITE_RECV0 + 2
};

/* Whether this install applies site i. The helper always; the inline words with
 * SHIM_INLINE_PID; the output sites only with SHIM_OUTPUT_HOOK and a row that
 * names the output sender (the stub tail-branches to orig_output, so a call site
 * patched with orig_output 0 would jump to address 0) - the call site and the
 * buffer slots in any output mode, the cap word only in probe (1) and full (2)
 * mode AND only with SHIM_INPUT_HOOK on, matching the out_mode the block carries
 * (anything else is observe, where nothing the kernel sends may change; the block
 * also carries out_mode 0 without the input hook, build_data_block). The cap word
 * lets the DualSense switch to its 79-byte full report; without the input redirect
 * (the bounce) the original transport read would copy that report into the pad
 * driver's 64-byte stack buffer, so the rewriting stages need the input hook and
 * the output hook only observes without it (the call site and the buffer slots
 * still apply, as in observe mode). The receive-limit words (sites 7-9) are
 * selected exactly with the cap word, and STRUCTURALLY only together with both
 * buffer slots: an 80-byte read into the kernel's packet buffer, left where it
 * was, would run into the HID client pointer behind it (0x432710). So they are
 * defined as the conjunction of the three sites they depend on, never from the
 * config on its own. The power-off sites (10-13) are independent of the output hook:
 * site 10 (the power-off call) with SHIM_POWEROFF_HOOK on and a row that names the
 * power-off sender (the stub tail-branches to it), sites 11-13 (the teardown
 * observers) additionally only outside the act stage - anything but
 * SHIM_POWEROFF_MODE 1 is observe, an out-of-range value included, as in the data
 * block's po_mode (poweroff_act) - and with a row that names the teardown routine.
 * With the hook off the install is exactly what data-block version 7 produced. */
static int poweroff_act(void)
{
	return g_cfg.shim_poweroff_hook != 0 && g_cfg.shim_poweroff_mode == 1;
}

static int site_selected(const ds45_shim_kernel_t *k, int i)
{
	if (i < DS45_SHIM_PID_PATCHES)
		return 1;
	if (i < DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES)
		return g_cfg.shim_inline_pid != 0;
	if (i >= SITE_PO0) {
		if (!g_cfg.shim_poweroff_hook)
			return 0;
		if (i == SITE_PO0)
			return k->orig_poweroff != 0;
		return !poweroff_act() && k->orig_teardown != 0;
	}
	if (site_is_recv(i))
		return site_selected(k, SITE_BUF) && site_selected(k, SITE_BUF1) &&
		       site_selected(k, SITE_OUT_CAP);
	if (!g_cfg.shim_output_hook || k->orig_output == 0)
		return 0;
	if (i == SITE_OUT_CAP)
		return g_cfg.shim_input_hook != 0 &&
		       (g_cfg.shim_output_mode == 1 || g_cfg.shim_output_mode == 2);
	return 1;
}

/* Whether an adopter may restore site i that the adopted install recorded as
 * applied: the row must populate it (a non-zero kind), and for an output or receive
 * site the row must still name the output sender, for the power-off site (10) the
 * power-off sender and for a teardown site (11-13) the teardown routine.
 * Deliberately NOT the current config: what the install applied is what gets
 * restored, like the inline words. */
static int site_restorable(const ds45_shim_kernel_t *k, int i)
{
	if (pid_site(k, i)->kind == 0)
		return 0;
	if (i >= SITE_PO0)
		return (i == SITE_PO0 ? k->orig_poweroff : k->orig_teardown) != 0;
	if (i >= DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES && k->orig_output == 0)
		return 0;
	return 1;
}

/* Whether the qword q is what row p was researched against: a PTR row compares
 * the value, every other kind the FNV of the qword. */
static int site_matches(const ds45_kpatch_t *p, uint64_t q)
{
	if (p->kind == DS45_KPATCH_PTR)
		return q == p->expect;
	return qword_hash(q) == p->expect;
}

/* The product-ID rows this install applies (site_selected) into out[], in
 * PID_ORDER (how many is returned in *n; site[] names each one's site index; the
 * receive words, the tail, are moved behind the redirects by kshim_install_from).
 * Each site must still hold what its row was researched against; its original
 * qword goes into orig[site] and the site's bit into *applied, for the data
 * block. (A branch row's new instruction is not known to kpatch until the blob's
 * load base is known, so the check is made here, ahead of the blob placement, as
 * well as by the final kpatch_apply.) 0 if any site differs or cannot be read. */
static int pid_capture(const ds45_shim_kernel_t *k, ds45_kpatch_t *out, int site[],
                       int *n, uint64_t orig[DS45_SHIM_PID_SITES], uint32_t *applied)
{
	const ds45_kpatch_t *p;
	uint64_t cur;
	int i, j;

	*n = 0;
	*applied = 0;
	for (i = 0; i < DS45_SHIM_PID_SITES; i++)
		orig[i] = 0;
	for (j = 0; j < DS45_SHIM_PID_SITES; j++) {
		i = PID_ORDER[j];
		if (!site_selected(k, i))
			continue;
		p = pid_site(k, i);
		/* a selected site the row does not populate fails the install (closed), it
		 * is never skipped: a receive row skipped would leave the buffer moved and
		 * the DualSense unable to report, an output row skipped a half-applied hook */
		if (p->kind == 0)
			return 0;
		/* a PTR slot is read at its own (8-aligned) address, an instruction row
		 * by the qword holding it */
		if (ds45_lv2_read(p->kind == DS45_KPATCH_PTR ? p->addr : (p->addr & ~7ULL),
		                  &cur) ||
		    !site_matches(p, cur))
			return 0;
		orig[i] = cur;
		*applied |= 1u << i;
		site[*n] = i;
		out[(*n)++] = *p;
	}
	return 1;
}

/* What row p of site `site` writes, once the blob is at base: the finished
 * instruction of a WORD row; shim_pid_ok for the BRANCH helper; the output entry
 * for the BRANCHL call site; the blob's packet buffer (site 5) and that plus one
 * (site 6) for the PTR slots; the power-off entry (site 10) or the teardown entry
 * (sites 11-13) for the power-off branches. The final transaction and adoption
 * both go through this, so an adopted install is recomputed exactly as it was
 * written. */
static uint64_t pid_value(int site, const ds45_kpatch_t *p, uint64_t base)
{
	/* the power-off sites (10-13) are branches into the blob whichever the kind (a
	 * `bl` or a `b`): site 10 aims at the power-off entry, sites 11-13 at the
	 * teardown entry */
	if (site >= SITE_PO0 &&
	    (p->kind == DS45_KPATCH_BRANCH || p->kind == DS45_KPATCH_BRANCHL))
		return base + (site == SITE_PO0 ? DS45_SHIM_OFF_POWEROFF : DS45_SHIM_OFF_TEARDOWN);
	switch (p->kind) {
	case DS45_KPATCH_BRANCH:
		return base + DS45_SHIM_OFF_PID_OK;
	case DS45_KPATCH_BRANCHL:
		return base + DS45_SHIM_OFF_OUTPUT;
	case DS45_KPATCH_PTR:
		return base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pktbuf) +
		       (site == SITE_BUF1 ? 1 : 0);
	default:
		return p->value;
	}
}

/* The qword product-ID site `site` holds once row p is applied over orig: a PTR
 * row replaces the whole qword; the other kinds' instruction replaces the high
 * half when (addr & 4) == 0, the low half otherwise (kpatch.h). The BRANCH and
 * BRANCHL rows target the blob at base (pid_value). 0 if the row's instruction
 * cannot be formed.
 * The BRANCH arm serves the helper row (tests/test_kshim.c
 * test_helper_branch_row), the BRANCHL and PTR arms the output sites. */
static int pid_patched_qword(int site, const ds45_kpatch_t *p, uint64_t orig,
                             uint64_t base, uint64_t *out)
{
	const uint64_t v = pid_value(site, p, base);
	uint32_t w;

	if (p->kind == DS45_KPATCH_PTR) {
		*out = v;
		return 1;
	}
	if (p->kind == DS45_KPATCH_WORD) {
		if (v > 0xFFFFFFFFULL)
			return 0;
		w = (uint32_t)v;
	} else if (p->kind == DS45_KPATCH_BRANCH) {
		if (kpatch_branch_word(p->addr, v, &w))
			return 0;
	} else if (p->kind == DS45_KPATCH_BRANCHL) {
		if (kpatch_branchl_word(p->addr, v, &w))
			return 0;
	} else {
		return 0;
	}
	*out = (p->addr & 4) ? ((orig & 0xFFFFFFFF00000000ULL) | w)
	                     : ((orig & 0x00000000FFFFFFFFULL) | ((uint64_t)w << 32));
	return 1;
}

/* The sites the adopted install recorded as applied (its data block's
 * pid_applied: bit 0 helper, bits 1-2 inline, bits 3-6 output, bits 7-9 receive
 * limits, bits 10-13 power-off); 0 if the block cannot be read (nothing is then
 * restored but the redirects). */
static uint32_t adopt_applied(uint64_t base)
{
	uint64_t q;
	uint32_t applied;

	if (ds45_lv2_read(base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_applied),
	                  &q)) {
		DS45_TRACE("kshim_adopt_applied_unreadable");
		return 0;
	}
	applied = (uint32_t)(q >> 32);          /* the high word of its big-endian qword */
	DS45_TRACE_HEX("kshim_adopt_applied", applied);
	return applied;
}

/* Whether site i, recorded as applied, is provably ours to restore: the row
 * populates it (site_restorable), the recorded original is what the row was
 * researched against, and the site still holds exactly that original with the
 * row's edit in it. Fills *orig. Reads only. */
static int adopt_site_ours(const ds45_shim_kernel_t *k, uint64_t base, int i,
                           uint64_t *orig)
{
	const ds45_kpatch_t *p;
	uint64_t cur, want;

	if (!site_restorable(k, i))
		return 0;
	p = pid_site(k, i);
	return !ds45_lv2_read(base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, pid_orig) +
	                      8 * (uint64_t)i, orig) &&
	       site_matches(p, *orig) &&
	       pid_patched_qword(i, p, *orig, base, &want) &&
	       !ds45_lv2_read(p->addr & ~7ULL, &cur) && cur == want;
}

/* The receive-limit sites (7-9) the adopted install recorded as applied that are
 * NOT ours now (adopt_site_ours fails: not exactly the patched value, a corrupt
 * recorded original, an unpopulated row, no sender address), as a mask of site
 * bits. A non-zero mask makes the adoption unsafe, and it is REFUSED as a whole
 * (KSHIM_FAILED, nothing written or journalled, the earlier blob left live): the
 * redirects (the input bounce) can only be taken away together with the raised
 * limits, and a limit we cannot lower must keep its bounce - restoring the input
 * slot to the original transport read under a limit of 0x50 lets a 79-byte
 * DualSense report overrun the pad driver's 64-byte stack buffer. Reads only. */
static uint32_t adopt_recv_unsafe(const ds45_shim_kernel_t *k, uint64_t base,
                                  uint32_t applied)
{
	uint64_t orig;
	uint32_t bad = 0;
	int i;

	for (i = SITE_RECV0; i < SITE_PO0; i++)
		if ((applied & (1u << i)) && !adopt_site_ours(k, base, i, &orig))
			bad |= 1u << i;
	return bad;
}

/* Adoption: journal the product-ID sites the adopted install recorded as applied
 * (`applied`), from the originals it kept in the data block - but only a site that
 * is provably ours (adopt_site_ours). The current config plays no part: what the
 * install applied is what gets restored. Any other site is left as found (traced
 * kshim_adopt_pid_skipped, not journalled); the redirects are adopted regardless.
 *
 * Two calls, mirroring an install's transaction: recv == 0 journals sites 0..6 and 10..13
 * (before the redirects), recv == 1 the receive-limit words (after them), so unload
 * undoes the limits first, then the redirects, then the rest. The receive words are
 * never skipped here: adopt_recv_unsafe has already refused the whole adoption if
 * any of them is not ours (see there). Reads only. Appends to j. */
static void adopt_pid_sites(const ds45_shim_kernel_t *k, uint64_t base,
                            uint32_t applied, int recv, ds45_kjournal_t *j)
{
	uint64_t orig;
	int i, jj;

	for (jj = 0; jj < DS45_SHIM_PID_SITES; jj++) {
		i = PID_ORDER[jj];
		if (!(applied & (1u << i)) || site_is_recv(i) != (recv != 0))
			continue;
		if (!adopt_site_ours(k, base, i, &orig)) {   /* not ours to restore: as found */
			DS45_TRACE_HEX("kshim_adopt_pid_skipped", (uint64_t)i);
			continue;
		}
		j->qaddr[j->n] = pid_site(k, i)->addr & ~7ULL;
		j->orig[j->n] = orig;
		j->n++;
	}
}

/* The redirects already point at one of our blobs: the event slot names a base,
 * the enable and input slots agree with it, and that base's data block carries
 * the magic and this build's version. A blob left by a build with another
 * data-block layout is not adopted: the plugin reaches into the block by this
 * build's offsets. Fills *base. */
static int already_installed(const ds45_shim_kernel_t *k, uint64_t *base)
{
	uint64_t ev, en, iv, b, magic, ver;

	if (ds45_lv2_read(k->event_slot, &ev) || ds45_lv2_read(k->enable_slot, &en) ||
	    ds45_lv2_read(k->input_slot, &iv))
		return 0;
	if (ev < DS45_SHIM_OFF_EVENT)
		return 0;
	b = ev - DS45_SHIM_OFF_EVENT;
	if (en != b + DS45_SHIM_OFF_ENABLE || iv != b + DS45_SHIM_OFF_INPUT)
		return 0;
	if (ds45_lv2_read(b + DS45_SHIM_OFF_DATA, &magic))
		return 0;
	if (magic != DS45_SHIM_MAGIC)
		return 0;
	/* the version is the high word of its big-endian qword (enabled follows) */
	if (ds45_lv2_read(b + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, version), &ver))
		return 0;
	if ((uint32_t)(ver >> 32) != DS45_SHIM_VERSION)
		return 0;
	*base = b;
	return 1;
}

/* Build the data block the plugin owns, in PPU (big-endian) byte order. magic
 * and version are set to the same constants the blob ships; the counters start
 * zeroed. Field offsets come from the shared ABI struct, which has identical
 * layout on the plugin, the blob and the host (fixed-width fields at natural
 * alignment, no pointers). */
static void build_data_block(const ds45_shim_kernel_t *k,
                             const uint64_t pid_orig[DS45_SHIM_PID_SITES],
                             uint32_t pid_applied, uint8_t *buf)
{
	uint8_t *cfg, *t;
	int i;

	memset(buf, 0, sizeof(ds45_shim_data_t));
	put_be64(buf + offsetof(ds45_shim_data_t, magic), DS45_SHIM_MAGIC);
	put_be32(buf + offsetof(ds45_shim_data_t, version), DS45_SHIM_VERSION);
	put_be32(buf + offsetof(ds45_shim_data_t, enabled), 1);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_event), k->event_expect);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_enable), k->enable_expect);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_input), k->input_expect);
	put_be64(buf + offsetof(ds45_shim_data_t, pad_vtable), k->pad_vtable);
	put_be64(buf + offsetof(ds45_shim_data_t, pad_mgr), k->pad_mgr);
	put_be64(buf + offsetof(ds45_shim_data_t, conn_tbl), k->conn_tbl);
	put_be32(buf + offsetof(ds45_shim_data_t, stable_id), g_cfg.shim_stable_id ? 1 : 0);
	/* the output hook's stage (0..2; anything else is observe) and the sender it
	 * tail-calls. Without SHIM_INPUT_HOOK the stage is 0 whatever the mode: the
	 * blob must never rewrite the output (and so never let the DualSense into its
	 * 79-byte report) when the input bounce that can receive it is not installed
	 * (site_selected leaves the cap word and the receive limits out as well). */
	put_be32(buf + offsetof(ds45_shim_data_t, out_mode),
	         (g_cfg.shim_input_hook &&
	          g_cfg.shim_output_mode >= 0 && g_cfg.shim_output_mode <= 2)
	             ? (uint32_t)g_cfg.shim_output_mode : 0);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_output), k->orig_output);
	/* the power-off hook's targets (the row's two code addresses, zero when it names
	 * none) and its stage: 1 (act) only with the hook on and SHIM_POWEROFF_MODE
	 * exactly 1, 0 (observe) for everything else - an out-of-range mode included,
	 * the same rule site_selected uses to leave the observers out */
	put_be64(buf + offsetof(ds45_shim_data_t, orig_poweroff), k->orig_poweroff);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_teardown), k->orig_teardown);
	put_be32(buf + offsetof(ds45_shim_data_t, po_mode), poweroff_act() ? 1 : 0);

	cfg = buf + offsetof(ds45_shim_data_t, cfg);
	cfg[offsetof(ds45_shim_cfg_t, deadzone)]     = (uint8_t)g_cfg.stick_deadzone;
	cfg[offsetof(ds45_shim_cfg_t, map_touchpad)] = (uint8_t)g_cfg.map_touchpad;
	cfg[offsetof(ds45_shim_cfg_t, map_mute)]     = (uint8_t)g_cfg.map_mute;
	cfg[offsetof(ds45_shim_cfg_t, sixaxis)]      = (uint8_t)g_cfg.sixaxis;
	cfg[offsetof(ds45_shim_cfg_t, swap_xy)]      = (uint8_t)g_cfg.swap_xy;
	cfg[offsetof(ds45_shim_cfg_t, invert_x)]     = (uint8_t)g_cfg.invert_x;
	cfg[offsetof(ds45_shim_cfg_t, invert_y)]     = (uint8_t)g_cfg.invert_y;
	cfg[offsetof(ds45_shim_cfg_t, invert_z)]     = (uint8_t)g_cfg.invert_z;
	cfg[offsetof(ds45_shim_cfg_t, invert_g)]     = (uint8_t)g_cfg.invert_g;
	put_be16(cfg + offsetof(ds45_shim_cfg_t, accel_scale),
	         (uint16_t)(int16_t)g_cfg.accel_scale);
	put_be16(cfg + offsetof(ds45_shim_cfg_t, gyro_scale),
	         (uint16_t)(int16_t)g_cfg.gyro_scale);

	/* the DS3 info template (identity bytes zero; the blob fills them per pad) */
	t = buf + offsetof(ds45_shim_data_t, info_tmpl);
	for (i = 0; i < DS45_SHIM_INFO_LEN; i++)
		t[i] = k->info_tmpl[i];

	/* what the word edits replace, so a later load that adopts this blob can
	 * switch them back on its unload (kernel RAM only) */
	for (i = 0; i < DS45_SHIM_PID_SITES; i++)
		put_be64(buf + offsetof(ds45_shim_data_t, pid_orig) + 8 * i, pid_orig[i]);
	put_be32(buf + offsetof(ds45_shim_data_t, pid_applied), pid_applied);
}

static void put_redirect(ds45_kpatch_t *p, uint64_t slot, uint64_t expect,
                         uint64_t value)
{
	p->kind = DS45_KPATCH_PTR;
	p->addr = slot;
	p->expect = expect;
	p->value = value;
}

/* ------------------------------------------------------------------------ */

kshim_result_t kshim_install_from(const ds45_shim_kernel_t *tab, int n)
{
	const ds45_shim_kernel_t *k = NULL;
	ds45_kpatch_t check[KSHIM_REDIRECTS + 1], final[KSHIM_FINAL_MAX];
	ds45_kpatch_t recv_rows[DS45_SHIM_RECV_PATCHES];
	ds45_kjournal_t j;
	uint8_t db[sizeof(ds45_shim_data_t)];
	uint64_t spare_slot, base = 0, size, kernel_toc, flush_opd;
	uint64_t pid_orig[DS45_SHIM_PID_SITES];
	uint32_t off, bw, pid_applied;
	int pid_site_of[DS45_SHIM_PID_SITES];
	int i, r, npid, nfront, nfinal, pid_ok, spare_borrowed = 0;

	g_state = KSHIM_OFF;
	g_journal.n = 0;
	j.n = 0;

	/* 0. The ENABLE_SHIM gate. The shim patches live kernel code, so unless the
	 * config explicitly asks for it (ENABLE_SHIM = 1; the default is off) this does
	 * nothing at all: not a read, not a write, no allocation. A build that gets a
	 * patch wrong can then never take the console down at boot just by being
	 * deployed. */
	if (!g_cfg.enable_shim)
		return KSHIM_OFF;

	/* 1. match a row by fingerprint */
	if (tab)
		for (i = 0; i < n; i++)
			if (tab[i].fp_addr != 0 &&
			    kernel_fingerprint_matches(tab[i].fp_addr, tab[i].fp_hash)) {
				k = &tab[i];
				break;
			}
	if (!k) {
		DS45_TRACE("kshim_no_entry");
		return KSHIM_NO_ENTRY;
	}
	if (!k->info_tmpl) {
		DS45_TRACE("kshim_row_no_template");
		return KSHIM_FAILED;
	}

	spare_slot = k->syscall_table + (uint64_t)k->spare_syscall * 8;

	/* 2. verify the clean originals: the three redirects and the spare slot by
	 * value, the two handlers they lead to by their first code qword, and the
	 * product-ID sites by the qword holding each instruction (whose originals
	 * are kept for the data block). */
	put_redirect(&check[0], k->event_slot, k->event_expect, 0);
	put_redirect(&check[1], k->enable_slot, k->enable_expect, 0);
	put_redirect(&check[2], k->input_slot, k->input_expect, 0);
	put_redirect(&check[3], spare_slot, k->spare_slot_expect, 0);

	r = kpatch_verify(check, KSHIM_REDIRECTS + 1);
	pid_ok = pid_capture(k, final, pid_site_of, &npid, pid_orig, &pid_applied);
	if (r != 0 ||
	    !hash_ok(k->event_code_addr, k->event_code_hash) ||
	    !hash_ok(k->enable_code_addr, k->enable_code_hash) ||
	    !pid_ok) {
		/* A failed verify may just mean the shim is already installed from an
		 * earlier load: adopt it and write nothing. Its journal is rebuilt as an
		 * install would have left it - the product-ID words it recorded as
		 * applied (each only if still provably ours), then the redirects, then
		 * the receive-limit words - so unload undoes the earlier load completely,
		 * the limits first. */
		if (already_installed(k, &base)) {
			const uint32_t applied = adopt_applied(base);
			const uint32_t unsafe = adopt_recv_unsafe(k, base, applied);

			/* Refuse BEFORE any journal entry or state is touched: the earlier
			 * blob stays live as it is, with its bounce and its limits consistent;
			 * g_state stays KSHIM_OFF, so neither the unload nor the scrub acts on
			 * it (adopt_recv_unsafe). */
			if (unsafe) {
				DS45_TRACE_HEX("kshim_adopt_refused", unsafe);
				g_journal.n = 0;
				g_base = 0;
				g_zero_size = 0;
				g_scrub_armed = 0;
				return KSHIM_FAILED;
			}
			g_journal.n = 0;
			adopt_pid_sites(k, base, applied, 0, &g_journal);
			g_journal.qaddr[g_journal.n] = k->event_slot;
			g_journal.orig[g_journal.n++] = k->event_expect;
			g_journal.qaddr[g_journal.n] = k->enable_slot;
			g_journal.orig[g_journal.n++] = k->enable_expect;
			g_journal.qaddr[g_journal.n] = k->input_slot;
			g_journal.orig[g_journal.n++] = k->input_expect;
			adopt_pid_sites(k, base, applied, 1, &g_journal);
			/* The adopted base came from the (consistency-checked) event slot,
			 * but that is still a free kernel read: make it the scrub target only
			 * when it is exactly this row's fixed region and fits under exec_bound.
			 * Otherwise adopt the install anyway (so unload still reverts the
			 * hooks) but never scrub a base we did not place - a stale-but-harmless
			 * blob must not block adoption, so this is not a failure. */
			g_base = base;
			g_zero_size = (uint64_t)DS45_SHIM_BLOB_LEN +
			              (k->flush_desc ? KSHIM_FLUSH_OPD_LEN : 0);
			if (base != k->exec_base || base > k->exec_bound ||
			    g_zero_size > k->exec_bound - base) {
				DS45_TRACE("kshim_adopt_base_unexpected");
				g_zero_size = 0;
			}
			g_scrub_armed = 0;
			g_state = KSHIM_ALREADY;
			DS45_TRACE("kshim_already");
			return KSHIM_ALREADY;
		}
		DS45_TRACE_HEX("kshim_verify_step", (uint64_t)r);
		DS45_TRACE("kshim_verify_FAILED");
		return KSHIM_FAILED;
	}

	/* 3. place the blob at the fixed executable base (design Amendment 2). The
	 * region is reserved by the CFW and reads as a zero fill; before any write,
	 * verify base is 8-aligned inside the window, the whole region
	 * [base, base + size) ends at or before exec_bound, and it currently reads
	 * zero - so nothing else is clobbered. No allocator is called. */
	base = k->exec_base;
	size = (uint64_t)DS45_SHIM_BLOB_LEN +
	       (k->flush_desc ? KSHIM_FLUSH_OPD_LEN : 0);
	if ((base & 7) || base < DS45_LV2_BASE ||
	    base > k->exec_bound || size > k->exec_bound - base) {
		DS45_TRACE("kshim_exec_window");
		goto fail;
	}
	for (off = 0; off < size; off += 8) {
		uint64_t zq;

		if (ds45_lv2_read(base + off, &zq) || zq != 0) {
			DS45_TRACE("kshim_exec_not_zero");
			goto fail;
		}
	}

	/* 4. a branch row must reach its target in the blob within a 26-bit
	 * displacement: the BRANCH helper shim_pid_ok, the BRANCHL output call site
	 * the output entry. Refused here, before the blob is written, if it cannot
	 * (out of range, or an address that cannot be encoded). A WORD row is a
	 * finished instruction and a PTR row a pointer; neither has a range to
	 * check. */
	for (i = 0; i < npid; i++) {
		if (final[i].kind != DS45_KPATCH_BRANCH && final[i].kind != DS45_KPATCH_BRANCHL)
			continue;
		r = final[i].kind == DS45_KPATCH_BRANCHL
		    ? kpatch_branchl_word(final[i].addr,
		                          pid_value(pid_site_of[i], &final[i], base), &bw)
		    : kpatch_branch_word(final[i].addr,
		                         pid_value(pid_site_of[i], &final[i], base), &bw);
		if (r) {
			DS45_TRACE("kshim_exec_range");
			goto fail;
		}
	}

	/* 5. copy the blob, rebasing every relocated word to the load base */
	for (off = 0; off < DS45_SHIM_BLOB_LEN; off += 8) {
		uint64_t q = be64(ds45_shim_blob + off);

		if (off_is_reloc(off))
			q += base;
		if (ds45_lv2_write(base + off, q)) {
			DS45_TRACE_HEX("kshim_blob_off", off);
			DS45_TRACE("kshim_blob_FAILED");
			goto fail;
		}
	}

	/* 6. the data block (originals, pad class and manager, template, cfg from
	 * g_cfg, enabled) */
	build_data_block(k, pid_orig, pid_applied, db);
	for (off = 0; off < sizeof(db); off += 8)
		if (ds45_lv2_write(base + DS45_SHIM_OFF_DATA + off, be64(db + off))) {
			DS45_TRACE("kshim_data_FAILED");
			goto fail;
		}

	/* 7. make the freshly written blob code coherent (F5), through the same
	 * borrowed spare slot, pointed at an OPD { flush entry, kernel TOC } we
	 * place just past the blob. The kernel TOC is read from the allocator's own
	 * OPD (+8). flush_desc == 0 would mean the poke is already coherent. */
	if (k->flush_desc) {
		if (ds45_lv2_read(k->alloc_desc + 8, &kernel_toc)) {
			DS45_TRACE("kshim_toc_FAILED");
			goto fail;
		}
		flush_opd = base + DS45_SHIM_BLOB_LEN;
		if (ds45_lv2_write(flush_opd, k->flush_desc) ||
		    ds45_lv2_write(flush_opd + 8, kernel_toc)) {
			DS45_TRACE("kshim_flush_opd_FAILED");
			goto fail;
		}
		spare_borrowed = 1;
		if (ds45_lv2_write(spare_slot, flush_opd)) {
			DS45_TRACE("kshim_flush_slot_FAILED");
			goto fail;
		}
		(void)kshim_spare_call(k->spare_syscall, base, DS45_SHIM_BLOB_LEN);
		if (ds45_lv2_write(spare_slot, k->spare_slot_expect)) {
			DS45_TRACE("kshim_flush_restore_FAILED");
			goto fail;
		}
		spare_borrowed = 0;
	}

	/* 8. the product-ID words and output sites, then the event, enable and input
	 * redirects, then the three receive-limit words - one transactional apply
	 * (verify all, write in order, revert on any failure; the revert, like an
	 * unload, goes last first, so the limits drop before the input bounce goes).
	 * The data block (orig_output included) was written in step 6, so it is
	 * complete before the first of these lands. A WORD row goes in exactly as the
	 * table gives it; the BRANCH helper is aimed at shim_pid_ok, the BRANCHL call
	 * site at the output entry, the PTR slots at the packet buffer in the blob
	 * (pid_value). A site or a slot that changed since step 2 fails this verify,
	 * so nothing is written. */
	for (i = 0; i < npid; i++)
		final[i].value = pid_value(pid_site_of[i], &final[i], base);
	/* The receive-limit words (sites 7-9, the PID_ORDER tail) are set aside and
	 * appended after the redirects, so they are the LAST writes of the transaction
	 * and, journalled last, the first reverts of an unload or a rollback: the
	 * limits drop back to 0x41 while the input bounce is still in place. */
	nfront = npid;
	while (nfront > 0 && site_is_recv(pid_site_of[nfront - 1]))
		nfront--;
	for (i = nfront; i < npid; i++)
		recv_rows[i - nfront] = final[i];
	nfinal = nfront;
	/* The event and enable redirects are each gated like the input one
	 * (SHIM_EVENT_HOOK / SHIM_ENABLE_HOOK); a gated-off slot is left original and
	 * omitted from the journal. All three off leaves only the product-ID word
	 * edits, to isolate the completion hooks from the helper patch on hardware. */
	if (g_cfg.shim_event_hook)
		put_redirect(&final[nfinal++], k->event_slot, k->event_expect,
		             base + DS45_SHIM_OFF_EVENT);
	if (g_cfg.shim_enable_hook)
		put_redirect(&final[nfinal++], k->enable_slot, k->enable_expect,
		             base + DS45_SHIM_OFF_ENABLE);
	/* The input redirect only when SHIM_INPUT_HOOK is set (design Amendment 3).
	 * With it off, the start-up and enable completion still install (helper, inline
	 * words, event and enable), the input slot is left original and omitted from
	 * the journal, and orig_input stays filled in the data block. The journal is
	 * then one entry shorter; rollback and unload are journal-driven and handle it
	 * unchanged. The step-2 verify of the input slot above is a read only, so it
	 * still confirms a clean kernel without writing. */
	if (g_cfg.shim_input_hook)
		put_redirect(&final[nfinal++], k->input_slot, k->input_expect,
		             base + DS45_SHIM_OFF_INPUT);
	for (i = 0; i < npid - nfront; i++)
		final[nfinal++] = recv_rows[i];

	r = kpatch_apply(final, nfinal, &j);
	if (r != 0) {
		/* kpatch_apply has already reverted its own partial writes. */
		DS45_TRACE_HEX("kshim_patch_step", (uint64_t)r);
		DS45_TRACE("kshim_patch_FAILED");
		goto fail;
	}

	g_journal = j;
	g_base = base;
	g_zero_size = size;
	g_scrub_armed = 0;        /* unhook arms the scrub once the revert is confirmed */
	g_state = KSHIM_INSTALLED;
	DS45_TRACE("kshim_installed");
	return KSHIM_INSTALLED;

fail:
	/* Restore the spare slot only if the flush actually borrowed it; the fixed
	 * blob region is scratch (not part of the pre-install image and not freed),
	 * and kpatch_apply has already undone any patch it half-applied, so there is
	 * nothing else to undo. A refusal before the flush writes nothing at all. */
	if (spare_borrowed)
		(void)ds45_lv2_write(spare_slot, k->spare_slot_expect);
	g_state = KSHIM_FAILED;
	g_journal.n = 0;
	return KSHIM_FAILED;
}

kshim_result_t kshim_install(void)
{
	return kshim_install_from(ds45_shim_table, ds45_shim_table_count);
}

/* Step 1 of unload: switch the redirects back and revert the product-ID words,
 * last first, each read back. A fresh install's journal has every patch it
 * applied - the helper, the inline words if any, and the three redirects; an
 * adopted one the same, rebuilt from the originals the adopted install recorded
 * (minus any site that was no longer provably ours).
 *
 * kpatch_revert swallows a failed write, so afterwards re-read every journalled
 * qword and compare to its recorded original. The scrub is armed only when they
 * all stuck: a redirect still pointing into the blob must not be followed by
 * zeroing that blob. */
void kshim_unhook(void)
{
	ds45_kjournal_t saved;
	uint64_t cur;
	int i, stuck = 1;

	if (g_state != KSHIM_INSTALLED && g_state != KSHIM_ALREADY)
		return;

	saved = g_journal;            /* kpatch_revert clears g_journal.n */
	kpatch_revert(&g_journal);

	for (i = 0; i < saved.n; i++)
		if (ds45_lv2_read(saved.qaddr[i], &cur) || cur != saved.orig[i]) {
			stuck = 0;
			break;
		}

	/* arm the scrub only when every redirect/word reverted AND there is a valid
	 * region to scrub (g_zero_size 0 = an adopted base we did not place) */
	if (stuck) {
		g_scrub_armed = g_zero_size != 0;
	} else {
		g_scrub_armed = 0;
		DS45_TRACE("kshim_remove_kept");
	}

	DS45_TRACE("kshim_unhooked");
	g_state = KSHIM_OFF;
	g_journal.n = 0;
}

/* Step 2 of unload: zero the fixed blob region [g_base, g_base + g_zero_size),
 * restoring the pristine fill a later install's zero-check needs (RAM-only; a
 * reboot clears it regardless). A no-op unless the preceding unhook armed it, so
 * it never zeroes a blob a still-live redirect points at. The caller drains any
 * in-blob thread before calling this. ds45_lv2_write reads each poke back, and a
 * read-back of the first and last qword confirms the ends landed. */
void kshim_scrub(void)
{
	uint64_t off, zq;

	if (!g_scrub_armed)
		return;
	g_scrub_armed = 0;

	for (off = 0; off < g_zero_size; off += 8)
		(void)ds45_lv2_write(g_base + off, 0);
	if ((ds45_lv2_read(g_base, &zq) || zq != 0) ||
	    (g_zero_size >= 8 &&
	     (ds45_lv2_read(g_base + g_zero_size - 8, &zq) || zq != 0)))
		DS45_TRACE("kshim_remove_not_zero");

	DS45_TRACE("kshim_removed");
}

#ifdef DS45_DEBUG
/* Debug tracing (design Amendment 3): the base of the installed data block, so
 * the plugin's housekeeping can read the diagnostic counters back with
 * ds45_lv2_read. 1 and *data_base = g_base + DS45_SHIM_OFF_DATA when a fresh or
 * adopted install is live; 0 otherwise (and *data_base is left alone). */
int kshim_dbg_data_base(uint64_t *data_base)
{
	if (g_state != KSHIM_INSTALLED && g_state != KSHIM_ALREADY)
		return 0;
	if (data_base)
		*data_base = g_base + DS45_SHIM_OFF_DATA;
	return 1;
}

/* Executability pre-flight. See kshim.h for the return codes. The leaf stub goes
 * at exec_base, with the flush OPD and the exec OPD just past it in the reserved
 * region (a 40-byte scratch span). Pre-poke checks fit/slot/zero; a single clean
 * exit restores the spare slot and zeroes + verifies the span, so a 0 return
 * proves the region is clean. It traces only exec_probe_call (right before the
 * stub call, so a boot trace that stops at the flush vs at the call tells a
 * flush-stage hang from a stub fault); main.c traces *out and the return value. */
int kshim_exec_probe(const ds45_shim_kernel_t *k, uint64_t *out)
{
	const uint32_t magic = 0x5AED;
	uint64_t base       = k->exec_base;
	uint64_t spare_slot = k->syscall_table + (uint64_t)k->spare_syscall * 8;
	uint64_t flush_opd  = base + 8;                       /* { flush entry, TOC } */
	uint64_t exec_opd   = base + 8 + KSHIM_FLUSH_OPD_LEN; /* { exec_base,  TOC } */
	uint64_t span       = 8 + 2 * (uint64_t)KSHIM_FLUSH_OPD_LEN;   /* stub + 2 OPDs */
	uint64_t kernel_toc, cur, zq, stub, off;
	int rc = 0;

	if (out)
		*out = 0;

	/* Pre-poke checks (write and borrow nothing on failure): base aligned and in
	 * window with the whole 40-byte span under exec_bound (overflow-safe); the
	 * spare slot still the ni stub; the whole span reads zero. */
	if ((base & 7) || base < DS45_LV2_BASE ||
	    base > k->exec_bound || span > k->exec_bound - base)
		return 1;
	if (ds45_lv2_read(spare_slot, &cur) || cur != k->spare_slot_expect)
		return 2;
	for (off = 0; off < span; off += 8)
		if (ds45_lv2_read(base + off, &zq) || zq != 0)
			return 3;
	/* the kernel TOC the flush and exec OPDs need (the allocator OPD's +8) */
	if (ds45_lv2_read(k->alloc_desc + 8, &kernel_toc))
		return 4;

	/* li r3,<magic> ; blr - one big-endian qword */
	stub = ((uint64_t)(0x38600000u | magic) << 32) | 0x4E800020u;
	if (ds45_lv2_write(base, stub)) {
		rc = 5;
		goto cleanup;
	}

	/* flush the 8 stub bytes through the borrowed spare slot, exactly like the
	 * install flush: point the slot at { flush entry, TOC }, call, restore it */
	if (k->flush_desc) {
		if (ds45_lv2_write(flush_opd, k->flush_desc) ||
		    ds45_lv2_write(flush_opd + 8, kernel_toc) ||
		    ds45_lv2_write(spare_slot, flush_opd)) {
			rc = 6;
			goto cleanup;
		}
		(void)kshim_spare_call(k->spare_syscall, base, 8);
		if (ds45_lv2_write(spare_slot, k->spare_slot_expect)) {
			rc = 6;
			goto cleanup;
		}
	}

	/* point the spare slot at { exec_base, TOC } and call the stub once - the
	 * whole point of the probe; the stub is in place now, so make the call */
	if (ds45_lv2_write(exec_opd, base) ||
	    ds45_lv2_write(exec_opd + 8, kernel_toc) ||
	    ds45_lv2_write(spare_slot, exec_opd)) {
		rc = 7;
		goto cleanup;
	}
	DS45_TRACE("exec_probe_call");
	if (out)
		*out = kshim_spare_call(k->spare_syscall, 0, 0);   /* set *out before the restore */
	else
		(void)kshim_spare_call(k->spare_syscall, 0, 0);

cleanup:
	/* Single exit: restore the borrowed spare slot (retry once, like install's
	 * fail:), then zero the stub and both scratch OPDs and verify. Reached for a
	 * clean run (rc 0) and for codes 5-7; codes 1-4 returned before any write. */
	if (ds45_lv2_write(spare_slot, k->spare_slot_expect))
		(void)ds45_lv2_write(spare_slot, k->spare_slot_expect);
	for (off = 0; off < span; off += 8)
		(void)ds45_lv2_write(base + off, 0);
	for (off = 0; off < span; off += 8)
		if (ds45_lv2_read(base + off, &zq) || zq != 0) {
			if (rc == 0)
				rc = 7;
			break;
		}
	return rc;
}
#endif /* DS45_DEBUG */
