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
 *   4. range-check a BRANCH row against base (should a row carry one it must
 *      reach shim_pid_ok - WORD rows hold a finished instruction, no range);
 *   5. copy the blob qword by qword, rebasing relocated words;
 *   6. fill the data block (originals, pad class and manager, info template,
 *      cfg, enabled, and each product-ID site's original qword with the set of
 *      sites applied - in kernel RAM before the first word edit, so a later
 *      load that adopts the blob can undo the edits);
 *   7. flush the instruction cache over the blob (F5), by borrowing the spare
 *      syscall slot once (its only use here), pointed at { flush entry, TOC };
 *   8. apply, transactionally, the helper word, the inline words (only with
 *      SHIM_INLINE_PID), the event redirect, the enable redirect and the input
 *      redirect, in that order.
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
 * enabled), then the event, enable and input redirects. The journal holds one
 * entry for each. */
#define KSHIM_REDIRECTS 3
#define KSHIM_FINAL_MAX (DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES + KSHIM_REDIRECTS)
typedef char kshim_final_fits_kpatch[(KSHIM_FINAL_MAX <= DS45_KPATCH_MAX) ? 1 : -1];

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
typedef char kshim_pid_sites_match[(DS45_SHIM_PID_PATCHES + DS45_SHIM_INLINE_PATCHES ==
                                    DS45_SHIM_PID_SITES) ? 1 : -1];
typedef char kshim_adopt_fits_journal[(DS45_SHIM_PID_SITES + KSHIM_REDIRECTS <=
                                       DS45_KPATCH_MAX) ? 1 : -1];
typedef char kshim_pid_words_aligned[(offsetof(ds45_shim_data_t, pid_orig) % 8 == 0 &&
                                      offsetof(ds45_shim_data_t, pid_applied) % 8 == 0) ? 1 : -1];

/* What a successful (or adopted) install left behind, for kshim_remove. g_base
 * and g_zero_size are the fixed blob region [g_base, g_base + g_zero_size) that
 * unload zeroes; both are set on a fresh install and on adoption (recording the
 * size is cleaner than recomputing it from a journalled slot at unload, when the
 * row - and so exec_base and flush_desc - is no longer in hand). */
static ds45_kjournal_t g_journal;
static kshim_result_t  g_state = KSHIM_OFF;
static uint64_t        g_base;
static uint64_t        g_zero_size;

/* ------------------------------------------------------------------------ */

/* Issue the borrowed spare syscall. The number is a table field, so it is a
 * runtime value; the SDK macro loads it into r11, which a runtime value is
 * fine for. The allocator's result (a kernel pointer) comes back in r3. */
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

/* Product-ID site i of a row, in data-block order: the helper rows, then the
 * inline rows. */
static const ds45_kpatch_t *pid_site(const ds45_shim_kernel_t *k, int i)
{
	return i < DS45_SHIM_PID_PATCHES ? &k->pid_patches[i]
	                                 : &k->inline_patches[i - DS45_SHIM_PID_PATCHES];
}

/* The product-ID rows this install applies - the helper always, the inline
 * words only with SHIM_INLINE_PID - into out[] (how many is returned in *n).
 * Each site must still hold the qword its row was researched against; its
 * original qword goes into orig[site] and the site's bit into *applied, for the
 * data block. (A BRANCH row's new instruction is not known to kpatch until the
 * blob is allocated, so the hash is checked here, ahead of the allocation, as
 * well as by the final kpatch_apply.) 0 if any site differs or cannot be read. */
static int pid_capture(const ds45_shim_kernel_t *k, ds45_kpatch_t *out, int *n,
                       uint64_t orig[DS45_SHIM_PID_SITES], uint32_t *applied)
{
	const ds45_kpatch_t *p;
	uint64_t cur;
	int i;

	*n = 0;
	*applied = 0;
	for (i = 0; i < DS45_SHIM_PID_SITES; i++) {
		orig[i] = 0;
		if (i >= DS45_SHIM_PID_PATCHES && !g_cfg.shim_inline_pid)
			continue;
		p = pid_site(k, i);
		if (ds45_lv2_read(p->addr & ~7ULL, &cur) || qword_hash(cur) != p->expect)
			return 0;
		orig[i] = cur;
		*applied |= 1u << i;
		out[(*n)++] = *p;
	}
	return 1;
}

/* The qword a product-ID site holds once row p is applied over orig: the
 * instruction replaces the high half when (addr & 4) == 0, the low half
 * otherwise (kpatch.h). A BRANCH row targets shim_pid_ok in the blob at base.
 * 0 if the row's instruction cannot be formed.
 * The BRANCH arm here (and the BRANCH range check and retargeting in
 * kshim_install_from) has no row and no test today: stage 1 is WORD rows only.
 * It is kept on purpose for stage 1b, which may add a BRANCH row to
 * shim_pid_ok - do not delete it; test it with that row. */
static int pid_patched_qword(const ds45_kpatch_t *p, uint64_t orig, uint64_t base,
                             uint64_t *out)
{
	uint32_t w;

	if (p->kind == DS45_KPATCH_WORD) {
		if (p->value > 0xFFFFFFFFULL)
			return 0;
		w = (uint32_t)p->value;
	} else if (p->kind == DS45_KPATCH_BRANCH) {
		if (kpatch_branch_word(p->addr, base + DS45_SHIM_OFF_PID_OK, &w))
			return 0;
	} else {
		return 0;
	}
	*out = (p->addr & 4) ? ((orig & 0xFFFFFFFF00000000ULL) | w)
	                     : ((orig & 0x00000000FFFFFFFFULL) | ((uint64_t)w << 32));
	return 1;
}

/* Adoption: journal every product-ID site the adopted install recorded as
 * applied, from the original it kept in the data block - but only a site whose
 * recorded original hashes to the row's expected value AND which still holds
 * exactly that original with the row's instruction in it. Any other site is left
 * as found (traced, not journalled); the redirects are adopted regardless. Reads
 * only. Appends to j. */
static void adopt_pid_sites(const ds45_shim_kernel_t *k, uint64_t base,
                            ds45_kjournal_t *j)
{
	const uint64_t db = base + DS45_SHIM_OFF_DATA;
	const ds45_kpatch_t *p;
	uint64_t q, orig, cur, want;
	uint32_t applied;
	int i;

	if (ds45_lv2_read(db + offsetof(ds45_shim_data_t, pid_applied), &q)) {
		DS45_TRACE("kshim_adopt_applied_unreadable");
		return;
	}
	applied = (uint32_t)(q >> 32);          /* the high word of its big-endian qword */
	/* which word edits the adopted install recorded as live (bit 0 helper,
	 * bits 1-2 inline); kshim_adopt_pid_skipped names any not restorable */
	DS45_TRACE_HEX("kshim_adopt_applied", applied);
	for (i = 0; i < DS45_SHIM_PID_SITES; i++) {
		if (!(applied & (1u << i)))
			continue;
		p = pid_site(k, i);
		if (ds45_lv2_read(db + offsetof(ds45_shim_data_t, pid_orig) + 8 * (uint64_t)i,
		                  &orig) ||
		    qword_hash(orig) != p->expect ||
		    !pid_patched_qword(p, orig, base, &want) ||
		    ds45_lv2_read(p->addr & ~7ULL, &cur) || cur != want) {
			DS45_TRACE_HEX("kshim_adopt_pid_skipped", (uint64_t)i);
			continue;
		}
		j->qaddr[j->n] = p->addr & ~7ULL;
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
	ds45_kjournal_t j;
	uint8_t db[sizeof(ds45_shim_data_t)];
	uint64_t spare_slot, base = 0, size, kernel_toc, flush_opd, target;
	uint64_t pid_orig[DS45_SHIM_PID_SITES];
	uint32_t off, bw, pid_applied;
	int i, r, npid, nfinal, pid_ok, spare_borrowed = 0;

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
	pid_ok = pid_capture(k, final, &npid, pid_orig, &pid_applied);
	if (r != 0 ||
	    !hash_ok(k->event_code_addr, k->event_code_hash) ||
	    !hash_ok(k->enable_code_addr, k->enable_code_hash) ||
	    !pid_ok) {
		/* A failed verify may just mean the shim is already installed from an
		 * earlier load: adopt it and write nothing. Its journal is rebuilt as an
		 * install would have left it - the product-ID words it recorded as
		 * applied (each only if still provably ours), then the redirects - so
		 * unload undoes the earlier load completely. */
		if (already_installed(k, &base)) {
			g_journal.n = 0;
			adopt_pid_sites(k, base, &g_journal);
			g_journal.qaddr[g_journal.n] = k->event_slot;
			g_journal.orig[g_journal.n++] = k->event_expect;
			g_journal.qaddr[g_journal.n] = k->enable_slot;
			g_journal.orig[g_journal.n++] = k->enable_expect;
			g_journal.qaddr[g_journal.n] = k->input_slot;
			g_journal.orig[g_journal.n++] = k->input_expect;
			/* the fixed region this adopted blob sits in, so unload zeroes it */
			g_base = base;
			g_zero_size = (uint64_t)DS45_SHIM_BLOB_LEN +
			              (k->flush_desc ? KSHIM_FLUSH_OPD_LEN : 0);
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

	/* 4. a BRANCH row (none today; kept for stage 1b - see pid_patched_qword)
	 * must reach shim_pid_ok in the blob within a 26-bit branch. A WORD row is a
	 * finished instruction that never points at the blob, so it has no range to
	 * check. */
	target = base + DS45_SHIM_OFF_PID_OK;
	for (i = 0; i < npid; i++)
		if (final[i].kind == DS45_KPATCH_BRANCH &&
		    kpatch_branch_word(final[i].addr, target, &bw)) {
			DS45_TRACE("kshim_exec_range");
			goto fail;
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

	/* 8. the product-ID words, then the event, enable and input redirects - one
	 * transactional apply (verify all, write in order, revert on any failure).
	 * A WORD row goes in exactly as the table gives it; a BRANCH row would be
	 * aimed at shim_pid_ok. A site or a slot that changed since step 2 fails
	 * this verify, so nothing is written. */
	for (i = 0; i < npid; i++)
		if (final[i].kind == DS45_KPATCH_BRANCH)
			final[i].value = target;
	nfinal = npid;
	put_redirect(&final[nfinal++], k->event_slot, k->event_expect,
	             base + DS45_SHIM_OFF_EVENT);
	put_redirect(&final[nfinal++], k->enable_slot, k->enable_expect,
	             base + DS45_SHIM_OFF_ENABLE);
	put_redirect(&final[nfinal++], k->input_slot, k->input_expect,
	             base + DS45_SHIM_OFF_INPUT);

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

void kshim_remove(void)
{
	uint64_t off, zq;

	if (g_state != KSHIM_INSTALLED && g_state != KSHIM_ALREADY)
		return;
	/* Switch the redirects back and revert the product-ID words, last first,
	 * each read back. A fresh install's journal has every patch it applied - the
	 * helper, the inline words if any, and the three redirects; an adopted one
	 * the same, rebuilt from the originals the adopted install recorded (minus
	 * any site that was no longer provably ours). The redirects go back first, so
	 * no driver thread enters the blob after this. */
	kpatch_revert(&g_journal);

	/* Then zero the fixed blob region [g_base, g_base + g_zero_size). A fixed
	 * region cannot be freed; zeroing restores the pristine fill a later
	 * install's zero-check needs (RAM-only; a reboot clears it regardless).
	 * ds45_lv2_write reads each poke back, and a read-back of the first and last
	 * qword confirms the ends landed. */
	for (off = 0; off < g_zero_size; off += 8)
		(void)ds45_lv2_write(g_base + off, 0);
	if ((ds45_lv2_read(g_base, &zq) || zq != 0) ||
	    (g_zero_size >= 8 &&
	     (ds45_lv2_read(g_base + g_zero_size - 8, &zq) || zq != 0)))
		DS45_TRACE("kshim_remove_not_zero");

	DS45_TRACE("kshim_removed");
	g_state = KSHIM_OFF;
	g_journal.n = 0;
}

#ifdef DS45_DEBUG
/* Executability pre-flight. See kshim.h. The leaf stub goes at exec_base, with
 * the flush OPD and the exec OPD just past it in the reserved region; all of it
 * is zeroed again before returning, so the region is left pristine. It traces
 * nothing - main.c traces *out and the return value. */
int kshim_exec_probe(const ds45_shim_kernel_t *k, uint64_t *out)
{
	const uint32_t magic = 0x5AED;
	uint64_t base       = k->exec_base;
	uint64_t spare_slot = k->syscall_table + (uint64_t)k->spare_syscall * 8;
	uint64_t flush_opd  = base + 8;                       /* { flush entry, TOC } */
	uint64_t exec_opd   = base + 8 + KSHIM_FLUSH_OPD_LEN; /* { exec_base,  TOC } */
	uint64_t kernel_toc, zq, stub, ret = 0;

	if (out)
		*out = 0;

	/* the first qword of the region must read zero before we poke the stub */
	if (ds45_lv2_read(base, &zq) || zq != 0)
		return 1;
	/* the kernel TOC the flush and exec OPDs need (the allocator OPD's +8) */
	if (ds45_lv2_read(k->alloc_desc + 8, &kernel_toc))
		return 2;

	/* li r3,<magic> ; blr - one big-endian qword */
	stub = ((uint64_t)(0x38600000u | magic) << 32) | 0x4E800020u;
	if (ds45_lv2_write(base, stub))
		return 3;

	/* flush the 8 stub bytes through the borrowed spare slot, exactly like the
	 * install flush: point the slot at { flush entry, TOC }, call, restore it */
	if (k->flush_desc) {
		if (ds45_lv2_write(flush_opd, k->flush_desc) ||
		    ds45_lv2_write(flush_opd + 8, kernel_toc) ||
		    ds45_lv2_write(spare_slot, flush_opd)) {
			(void)ds45_lv2_write(spare_slot, k->spare_slot_expect);
			return 4;
		}
		(void)kshim_spare_call(k->spare_syscall, base, 8);
		if (ds45_lv2_write(spare_slot, k->spare_slot_expect))
			return 5;
	}

	/* point the spare slot at { exec_base, TOC } and call the stub once - the
	 * whole point of the probe; the stub is in place now, so make the call */
	if (ds45_lv2_write(exec_opd, base) ||
	    ds45_lv2_write(exec_opd + 8, kernel_toc) ||
	    ds45_lv2_write(spare_slot, exec_opd)) {
		(void)ds45_lv2_write(spare_slot, k->spare_slot_expect);
		return 6;
	}
	ret = kshim_spare_call(k->spare_syscall, 0, 0);
	if (ds45_lv2_write(spare_slot, k->spare_slot_expect))
		return 7;

	if (out)
		*out = ret;

	/* restore the pristine zero fill: the stub and both scratch OPDs */
	(void)ds45_lv2_write(base, 0);
	(void)ds45_lv2_write(flush_opd, 0);
	(void)ds45_lv2_write(flush_opd + 8, 0);
	(void)ds45_lv2_write(exec_opd, 0);
	(void)ds45_lv2_write(exec_opd + 8, 0);
	return 0;
}
#endif /* DS45_DEBUG */
