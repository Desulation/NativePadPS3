/*
 * DS45PAD - install the DS3-route kernel shim on a fingerprinted kernel.
 * See kshim.h, the fact sheet F1-F6 and the design spec's "Install".
 *
 * Install order (fixed):
 *   1. match a table row by fingerprint;
 *   2. verify the redirect slots, the spare syscall slot and the product-ID
 *      sites against their expected values - writing nothing if any differs,
 *      unless the redirects already point at one of our blobs (adopt it);
 *   3. borrow the spare syscall slot to call the LV2 allocator once, restore it;
 *   4. range-check the allocation (in-window, and a 26-bit branch of the F4 sites);
 *   5. copy the blob qword by qword, rebasing relocated words;
 *   6. fill the data block (originals, the transfer routine's descriptor, cfg,
 *      enabled);
 *   7. flush the instruction cache over the blob (F5);
 *   8. apply the product-ID branches and the two redirects transactionally.
 *
 * Every write is read back. Any failure restores the borrowed spare slot and
 * leaves every pre-existing kernel structure byte-identical to before (the
 * freshly-allocated blob region is scratch and is not freed; kpatch_apply undoes
 * its own partial writes). It is entirely RAM-only.
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

/* The data block exactly fills the blob's tail (OFF_DATA .. BLOB_LEN); it is
 * written qword by qword, so its size must be a multiple of 8. */
typedef char kshim_data_multiple_of_8[(sizeof(ds45_shim_data_t) % 8 == 0) ? 1 : -1];

/* Everything the plugin reaches into the live block for is a whole, aligned
 * qword: the version (adoption), the conn_state[] array - all of it, one byte
 * per slot - and the per-slot key, id and send-count words
 * (kshim_set_connection). */
typedef char kshim_version_qword[(offsetof(ds45_shim_data_t, version) % 8 == 0) ? 1 : -1];
typedef char kshim_conn_state_qword[(sizeof(((ds45_shim_data_t *)0)->conn_state) == 8 &&
                                     offsetof(ds45_shim_data_t, conn_state) % 8 == 0) ? 1 : -1];
typedef char kshim_conn_words_aligned[(offsetof(ds45_shim_data_t, conn_key) % 8 == 0 &&
                                       offsetof(ds45_shim_data_t, conn_id) % 8 == 0 &&
                                       offsetof(ds45_shim_data_t, start_tries) % 8 == 0) ? 1 : -1];

/* What a successful (or adopted) install left behind, for kshim_remove. */
static ds45_kjournal_t g_journal;
static uint64_t        g_base;
static kshim_result_t  g_state = KSHIM_OFF;

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

/* FNV-1a 64 of the 8-aligned qword containing addr, bytes in memory order (the
 * same scheme kpatch uses); 0 if the qword cannot be read. */
static int hash_ok(uint64_t addr, uint64_t expect)
{
	uint64_t cur;
	uint8_t b[8];
	int i;

	if (ds45_lv2_read(addr & ~7ULL, &cur))
		return 0;
	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(cur >> (56 - 8 * i));
	return ds45_fnv1a64(b, 8) == expect;
}

/* The redirects already point at one of our blobs: both slots consistent with a
 * single base and that base's data block carries the magic and this build's
 * version. A blob left by a build with another data-block layout is not adopted:
 * the plugin reaches into the block by this build's offsets. Fills *base. */
static int already_installed(const ds45_shim_kernel_t *k, uint64_t *base)
{
	uint64_t sv, iv, b, magic, ver;

	if (ds45_lv2_read(k->startup_slot, &sv) || ds45_lv2_read(k->input_slot, &iv))
		return 0;
	if (sv < DS45_SHIM_OFF_STARTUP)
		return 0;
	b = sv - DS45_SHIM_OFF_STARTUP;
	if (iv != b + DS45_SHIM_OFF_INPUT)
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
 * and version are set to the same constants the blob ships; the per-connection,
 * request-buffer and counter fields start zeroed. Field offsets come from the
 * shared ABI struct, which has identical layout on the plugin, the blob and the
 * host (fixed-width fields at natural alignment, no pointers). base is where the
 * blob was loaded: the transfer routine's descriptor lives in this block, so
 * send_routine holds an address inside the loaded copy. */
static void build_data_block(const ds45_shim_kernel_t *k, uint64_t base, uint8_t *buf)
{
	uint8_t *cfg;

	memset(buf, 0, sizeof(ds45_shim_data_t));
	put_be64(buf + offsetof(ds45_shim_data_t, magic), DS45_SHIM_MAGIC);
	put_be32(buf + offsetof(ds45_shim_data_t, version), DS45_SHIM_VERSION);
	put_be32(buf + offsetof(ds45_shim_data_t, enabled), 1);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_startup), k->startup_expect);
	put_be64(buf + offsetof(ds45_shim_data_t, orig_input), k->input_expect);

	/* The kernel's Bluetooth HID transfer routine has no descriptor of its own
	 * (it is called directly in-kernel, like the F5 flush routine), so the block
	 * carries one - ELFv1 { entry, toc, env }, env 0 - and send_routine is its
	 * address as loaded: the shim calls through send_routine exactly as it calls
	 * the originals. A row with no transfer routine leaves both zero, and the
	 * shim's DualSense start-up then fails safe. */
	if (k->send_entry) {
		put_be64(buf + offsetof(ds45_shim_data_t, send_opd), k->send_entry);
		put_be64(buf + offsetof(ds45_shim_data_t, send_opd) + 8, k->send_toc);
		put_be64(buf + offsetof(ds45_shim_data_t, send_routine),
		         base + DS45_SHIM_OFF_DATA + offsetof(ds45_shim_data_t, send_opd));
	}

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
}

/* ------------------------------------------------------------------------ */

kshim_result_t kshim_install_from(const ds45_shim_kernel_t *tab, int n)
{
	const ds45_shim_kernel_t *k = NULL;
	ds45_kpatch_t redir[3], final[4];
	ds45_kjournal_t j;
	uint8_t db[sizeof(ds45_shim_data_t)];
	uint64_t spare_slot, base = 0, alloc_size, kernel_toc, flush_opd, target;
	uint32_t off, bw;
	int i, r;

	g_state = KSHIM_OFF;
	g_journal.n = 0;
	g_base = 0;
	j.n = 0;

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

	spare_slot = k->syscall_table + (uint64_t)k->spare_syscall * 8;

	/* 2. verify the clean originals: the two redirects and the spare slot are
	 * pointers; the two product-ID sites are verified by the hash of the qword
	 * holding each patch instruction (their branch value is not known until the
	 * blob is allocated, so kpatch cannot encode them yet). */
	redir[0].kind = DS45_KPATCH_PTR;
	redir[0].addr = k->startup_slot;
	redir[0].expect = k->startup_expect;
	redir[0].value = 0;
	redir[1].kind = DS45_KPATCH_PTR;
	redir[1].addr = k->input_slot;
	redir[1].expect = k->input_expect;
	redir[1].value = 0;
	redir[2].kind = DS45_KPATCH_PTR;
	redir[2].addr = spare_slot;
	redir[2].expect = k->spare_slot_expect;
	redir[2].value = 0;

	r = kpatch_verify(redir, 3);
	if (r != 0 ||
	    !hash_ok(k->pid_patches[0].addr, k->pid_patches[0].expect) ||
	    !hash_ok(k->pid_patches[1].addr, k->pid_patches[1].expect)) {
		/* A failed verify may just mean the shim is already installed from an
		 * earlier load: adopt it and write nothing. */
		if (already_installed(k, &base)) {
			g_base = base;
			g_journal.n = 2;
			g_journal.qaddr[0] = k->startup_slot;
			g_journal.orig[0] = k->startup_expect;
			g_journal.qaddr[1] = k->input_slot;
			g_journal.orig[1] = k->input_expect;
			g_state = KSHIM_ALREADY;
			DS45_TRACE("kshim_already");
			return KSHIM_ALREADY;
		}
		DS45_TRACE_HEX("kshim_verify_step", (uint64_t)r);
		DS45_TRACE("kshim_verify_FAILED");
		return KSHIM_FAILED;
	}

	/* 3. borrow the spare slot for the LV2 allocator, then restore it */
	alloc_size = (uint64_t)DS45_SHIM_BLOB_LEN +
	             (k->flush_desc ? KSHIM_FLUSH_OPD_LEN : 0);
	if (ds45_lv2_write(spare_slot, k->alloc_desc)) {
		DS45_TRACE("kshim_alloc_slot_FAILED");
		goto fail;
	}
	base = kshim_spare_call(k->spare_syscall, alloc_size, k->alloc_pool);
	if (ds45_lv2_write(spare_slot, k->spare_slot_expect)) {
		DS45_TRACE("kshim_alloc_restore_FAILED");
		goto fail;
	}
	if (base == 0) {
		DS45_TRACE("kshim_alloc_zero");
		goto fail;
	}

	/* 4. the allocation must be writable (in-window, 8-aligned, whole region
	 * inside the bound) and within a 26-bit branch of both F4 sites */
	if ((base & 7) || base < DS45_LV2_BASE ||
	    base > DS45_LV2_BOUND - alloc_size) {
		DS45_TRACE("kshim_alloc_window");
		goto fail;
	}
	target = base + DS45_SHIM_OFF_PID_OK;
	if (kpatch_branch_word(k->pid_patches[0].addr, target, &bw) ||
	    kpatch_branch_word(k->pid_patches[1].addr, target, &bw)) {
		DS45_TRACE("kshim_alloc_range");
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

	/* 6. the data block (originals, the transfer routine's descriptor, cfg from
	 * g_cfg, enabled) */
	build_data_block(k, base, db);
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
		if (ds45_lv2_write(spare_slot, flush_opd)) {
			DS45_TRACE("kshim_flush_slot_FAILED");
			goto fail;
		}
		(void)kshim_spare_call(k->spare_syscall, base, DS45_SHIM_BLOB_LEN);
		if (ds45_lv2_write(spare_slot, k->spare_slot_expect)) {
			DS45_TRACE("kshim_flush_restore_FAILED");
			goto fail;
		}
	}

	/* 8. product-ID branches to shim_pid_ok, then the start-up and input
	 * redirects - one transactional apply (verify all, write in order, revert
	 * on any failure). A redirect that changed since step 2 fails this verify,
	 * so nothing is written. */
	final[0] = k->pid_patches[0];
	final[0].value = target;
	final[1] = k->pid_patches[1];
	final[1].value = target;
	final[2].kind = DS45_KPATCH_PTR;
	final[2].addr = k->startup_slot;
	final[2].expect = k->startup_expect;
	final[2].value = base + DS45_SHIM_OFF_STARTUP;
	final[3].kind = DS45_KPATCH_PTR;
	final[3].addr = k->input_slot;
	final[3].expect = k->input_expect;
	final[3].value = base + DS45_SHIM_OFF_INPUT;

	r = kpatch_apply(final, 4, &j);
	if (r != 0) {
		/* kpatch_apply has already reverted its own partial writes. */
		DS45_TRACE_HEX("kshim_patch_step", (uint64_t)r);
		DS45_TRACE("kshim_patch_FAILED");
		goto fail;
	}

	g_journal = j;
	g_base = base;
	g_state = KSHIM_INSTALLED;
	DS45_TRACE("kshim_installed");
	return KSHIM_INSTALLED;

fail:
	/* Restore the borrowed spare slot; the blob region is scratch (not part of
	 * the pre-install image and not freed), and kpatch_apply has already undone
	 * any patch it half-applied, so there is nothing else to undo. */
	(void)ds45_lv2_write(spare_slot, k->spare_slot_expect);
	g_state = KSHIM_FAILED;
	g_journal.n = 0;
	g_base = 0;
	return KSHIM_FAILED;
}

kshim_result_t kshim_install(void)
{
	return kshim_install_from(ds45_shim_table, ds45_shim_table_count);
}

/* Rewrite one byte of conn_state[], which is DS45_SHIM_CONNS bytes = one
 * big-endian qword (slot 0 its most significant byte), given the qword as just
 * read. Nothing but the plugin ever writes it (the blob only reads), so the
 * read-modify-write is safe. 0 once the write has read back, else -1. */
static int conn_state_put(uint64_t state_addr, uint64_t cur, int slot, uint8_t v)
{
	int sh = 56 - 8 * slot;

	return ds45_lv2_write(state_addr,
	                      (cur & ~(0xFFULL << sh)) | ((uint64_t)v << sh)) ? -1 : 0;
}

int kshim_set_connection(uint8_t handle, uint64_t conn_id)
{
	const uint64_t db = g_base + DS45_SHIM_OFF_DATA;
	const uint64_t state_addr = db + offsetof(ds45_shim_data_t, conn_state);
	uint64_t state, key, old_id;
	int i, slot = -1, free_slot = -1;

	if (g_state != KSHIM_INSTALLED && g_state != KSHIM_ALREADY)
		return -1;
	if (ds45_lv2_read(state_addr, &state))
		return -1;

	/* Find the slot the shim would match for this handle - the first one that
	 * is in use and holds it, as entry.c's lookup does - and the first free one. */
	for (i = 0; i < DS45_SHIM_CONNS; i++) {
		if (((state >> (56 - 8 * i)) & 0xFF) == 0) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (ds45_lv2_read(db + offsetof(ds45_shim_data_t, conn_key) + 8 * (uint64_t)i,
		                  &key))
			return -1;
		if ((uint8_t)key == handle) {
			slot = i;
			break;
		}
	}

	if (conn_id == 0) {
		/* Forget the handle: hide its slot from the shim first (its key and id
		 * are only read once the state byte is set, so they can stay). */
		if (slot < 0)
			return 0;
		if (conn_state_put(state_addr, state, slot, 0)) {
			DS45_TRACE("kshim_conn_forget_FAILED");
			return -1;
		}
		return 0;
	}

	if (slot >= 0) {
		/* The slot is live. The same id again is the same connection: nothing
		 * to write, and above all no fresh send budget - a caller that reports
		 * the connection on every rescan must not keep refilling it. */
		if (ds45_lv2_read(db + offsetof(ds45_shim_data_t, conn_id) + 8 * (uint64_t)slot,
		                  &old_id)) {
			DS45_TRACE("kshim_conn_read_FAILED");
			return -1;
		}
		if (old_id == conn_id)
			return 0;

		/* A different id: the handle has come back as a new connection. Its id
		 * is one aligned qword, which the shim reads in one piece; then its
		 * send budget is refilled, so the id is never paired with a budget the
		 * previous connection spent. */
		if (ds45_lv2_write(db + offsetof(ds45_shim_data_t, conn_id) + 8 * (uint64_t)slot,
		                   conn_id) ||
		    ds45_lv2_write(db + offsetof(ds45_shim_data_t, start_tries) + 8 * (uint64_t)slot,
		                   0)) {
			DS45_TRACE("kshim_conn_update_FAILED");
			return -1;
		}
		return 0;
	}

	if (free_slot < 0) {
		DS45_TRACE("kshim_conn_full");
		return -1;
	}

	/* A new connection: its id and handle, then a full send budget (the slot may
	 * hold the count its previous occupant left behind), and last the state
	 * byte, which is what makes the slot visible - so a write that does not
	 * stick never leaves the shim a half-written connection or a spent budget.
	 * The budget is a qword of its own, so zeroing it needs no read-modify-write
	 * that could undo the blob's count for a neighbouring slot. */
	if (ds45_lv2_write(db + offsetof(ds45_shim_data_t, conn_id) + 8 * (uint64_t)free_slot,
	                   conn_id) ||
	    ds45_lv2_write(db + offsetof(ds45_shim_data_t, conn_key) + 8 * (uint64_t)free_slot,
	                   handle) ||
	    ds45_lv2_write(db + offsetof(ds45_shim_data_t, start_tries) + 8 * (uint64_t)free_slot,
	                   0) ||
	    conn_state_put(state_addr, state, free_slot, 1)) {
		DS45_TRACE("kshim_conn_set_FAILED");
		return -1;
	}
	return 0;
}

void kshim_remove(void)
{
	if (g_state != KSHIM_INSTALLED && g_state != KSHIM_ALREADY)
		return;
	/* Switch the redirects back and revert the product-ID patches (a fresh
	 * install's journal has all four; an adopted one, only the redirects). The
	 * blob stays allocated - a driver thread may still be inside it. */
	kpatch_revert(&g_journal);
	DS45_TRACE("kshim_removed");
	g_state = KSHIM_OFF;
	g_journal.n = 0;
	g_base = 0;
}
