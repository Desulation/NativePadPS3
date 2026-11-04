/*
 * DS45PAD - install the DS3-route kernel shim on a fingerprinted kernel.
 *
 * On a kernel that matches a table entry, kshim_install() places the
 * freestanding shim blob (src/shim_blob.h) at a fixed base in executable kernel
 * memory (the CFW payload's free zero tail; design Amendment 2), fills
 * its data block (the original descriptors, the pad class and manager, the DS3
 * info template, the config), applies the product-ID word edits and points the
 * DS3 driver's start-up event handler, enable event handler and Bluetooth input
 * read at the blob. Every target is verified against its expected value first
 * (the slots by value, the two handlers by the hash of their first code qword),
 * every write is read back, and any failure undoes everything already written
 * so the kernel image is left exactly as it was found. It is entirely RAM-only
 * and runs only once, at load, on the VSH side - and only when the config
 * enables it (ENABLE_SHIM = 1; off by default), so a deployed build never
 * patches the kernel unasked.
 *
 * The one firmware row lives in kshim_table.c; the transactional engine is
 * src/kpatch.h and the bounded LV2 access is src/lv2.h. See the start-up
 * completion design spec (docs/superpowers/specs/2026-11-01-...) section 3 and
 * the facts note's C4 row values.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_KSHIM_H
#define DS45_KSHIM_H

#include <stdint.h>
#include "kpatch.h"

#define DS45_SHIM_PID_PATCHES     1   /* helper word, always */
#define DS45_SHIM_INLINE_PATCHES  2   /* inline words, only with SHIM_INLINE_PID = 1 */

/* One firmware. The fingerprint is the same (fp_addr, fp_hash) scheme
 * ds45_kernel_t uses, so a row is only ever applied to the kernel it was
 * researched against. The product-ID rows are WORD rows (a finished instruction
 * written as given); a BRANCH row would be aimed at shim_pid_ok.
 *
 * Every code hash (event_code_hash, enable_code_hash and each product-ID row's
 * expect) is the FNV-1a 64 of the 8-ALIGNED qword holding the address (addr & ~7,
 * its 8 bytes in memory order) - never of the 8 bytes starting at an unaligned
 * address. tests/test_kshim.c checks the built-in row against the pristine image
 * when it is available locally. */
typedef struct {
	const char *name;
	uint64_t fp_addr, fp_hash;
	uint64_t syscall_table; uint32_t spare_syscall; uint64_t spare_slot_expect;
	/* The LV2 allocator's OPD. No allocation is done any more (design Amendment
	 * 2): only its +8 is read, for the kernel TOC the flush and probe OPDs need. */
	uint64_t alloc_desc;
	/* The fixed executable region the blob is placed in, [exec_base, exec_bound)
	 * - on DEX 4.92 the CFW payload's free zero tail 0x57C000..0x580000. The blob
	 * (plus the flush OPD) is written at exec_base after the region is verified to
	 * read zero; a reboot clears it, unload zeroes it. */
	uint64_t exec_base, exec_bound;
	uint64_t flush_desc;                        /* 0 if the poke is coherent */
	uint64_t event_slot, event_expect;          /* 0x37CFF8 / 0x33A2B0 OPD */
	uint64_t event_code_addr, event_code_hash;  /* 0xCACF0 / 0x16E42A0B1EBFEFFA */
	uint64_t enable_slot, enable_expect;        /* 0x37CFB0 / 0x33A310 OPD */
	uint64_t enable_code_addr, enable_code_hash;/* 0xCB234 / 0xB52798EFCFE165A8 (qword 0xCB230) */
	uint64_t input_slot, input_expect;          /* 0x37CE38 / 0x33A010 OPD */
	uint64_t pad_vtable;                        /* 0x800000000038CDC8 */
	uint64_t pad_mgr;                           /* 0x80000000004A1CB8 */
	uint64_t conn_tbl;                          /* 0x8000000000432690: Bluetooth connection table (7 entry pointers, by handle) */
	const uint8_t *info_tmpl;                  /* DS45_SHIM_INFO_LEN bytes (facts note C1.5) */
	ds45_kpatch_t pid_patches[DS45_SHIM_PID_PATCHES];
	ds45_kpatch_t inline_patches[DS45_SHIM_INLINE_PATCHES];
} ds45_shim_kernel_t;
extern const ds45_shim_kernel_t ds45_shim_table[];
extern const int ds45_shim_table_count;

typedef enum { KSHIM_OFF = 0, KSHIM_INSTALLED, KSHIM_ALREADY, KSHIM_NO_ENTRY, KSHIM_FAILED } kshim_result_t;

/* Install over the built-in ds45_shim_table.
 *   KSHIM_OFF       the ENABLE_SHIM gate is shut (g_cfg.enable_shim == 0, the
 *                   default): nothing was read, nothing written, nothing allocated.
 *   KSHIM_NO_ENTRY  no fingerprint matched; nothing read-modified, nothing written.
 *   KSHIM_ALREADY   the event, enable and input slots already point at one blob
 *                   carrying DS45_SHIM_MAGIC and this build's version; the existing
 *                   install is adopted and nothing is written.
 *   KSHIM_INSTALLED the shim is now installed.
 *   KSHIM_FAILED    a target was not as expected, the fixed exec region did not
 *                   read zero or did not fit under exec_bound, or a write did not
 *                   read back; everything already written is undone and the kernel
 *                   image is byte-identical to before.
 * The two inline product-ID words are applied only when g_cfg.shim_inline_pid is
 * set (config key SHIM_INLINE_PID). Debug builds trace the failing step. */
kshim_result_t kshim_install(void);

/* The same over a caller-supplied table (tab may be NULL, n may be 0); this is
 * what the host tests drive with a synthetic row. kshim_install() is exactly
 * kshim_install_from(ds45_shim_table, ds45_shim_table_count). */
kshim_result_t kshim_install_from(const ds45_shim_kernel_t *tab, int n);

/* Switch the three redirects back and revert the product-ID words, from the
 * journal. A fresh install journals what it wrote. An adopted one rebuilds the
 * journal from the data block: the install records each edited site's original
 * qword (pid_orig) and which sites it applied (pid_applied), and adoption
 * journals a site only if that original hashes to the row's expected value and
 * the site still holds it with the row's instruction applied; any other site is
 * left as found.
 *
 * Unload is two steps, because the fixed blob region cannot be freed and a driver
 * thread may still be executing inside it:
 *   kshim_unhook() switches the redirects and words back from the journal and
 *   reads every reverted qword back; only if they all stuck does it ARM the scrub
 *   (a still-live redirect would make zeroing the blob fatal). It traces
 *   kshim_remove_kept and arms nothing if a revert did not read back. A no-op when
 *   nothing is installed.
 *   kshim_scrub() zeroes the fixed blob region [base, base + blob + flush OPD),
 *   restoring the pristine fill a later install's zero-check needs. It does
 *   nothing unless the preceding unhook armed it. RAM-only; a reboot clears it.
 * The caller drains any in-blob thread between the two (bounded, straight-line
 * blob code); the host tests, with no such thread, call them back to back. */
void kshim_unhook(void);
void kshim_scrub(void);

#ifdef DS45_DEBUG
/* Debug tracing (design Amendment 3): the base of the installed shim data block
 * (g_base + DS45_SHIM_OFF_DATA), so the plugin's debug housekeeping can read the
 * diagnostic counters back with ds45_lv2_read at base + the ABI field offsets.
 * Returns 1 and sets *data_base when a fresh or adopted install is live, 0
 * otherwise. Debug builds only. */
int kshim_dbg_data_base(uint64_t *data_base);

/* Executability pre-flight (design Amendment 2, console step 0): poke a two-word
 * leaf stub `li r3,<magic>; blr` at k->exec_base, flush it, point the borrowed
 * spare slot at an OPD { exec_base, kernel TOC } and call it once, and report
 * what it returned in *out (expect 0x5AED, proving the region executes).
 *
 * Before the first poke it verifies k->exec_base is 8-aligned in the window, the
 * 40-byte scratch span (stub + two scratch OPDs) fits under exec_bound, the spare
 * slot holds spare_slot_expect, and the whole span reads zero - any failing
 * writes and borrows nothing. Return value:
 *   0  success: the stub ran, *out holds its result, the span is zeroed again;
 *   1  exec_base misaligned/out of window, or the 40-byte span past exec_bound;
 *   2  the spare slot did not hold spare_slot_expect;
 *   3  the 40-byte scratch span did not read all zero;
 *   4  the kernel TOC (alloc_desc + 8) could not be read;
 *   5  the stub poke did not read back;
 *   6  a flush-stage write or the spare-slot borrow/restore did not read back;
 *   7  an exec-stage write/borrow did not read back, or the span could not be
 *      zeroed afterwards.
 * On any non-zero return from code 5 on, the spare slot is restored and the span
 * is zeroed (best effort); *out is 0 unless the call was made. Debug builds only;
 * it installs nothing and the kernel is not otherwise touched. main.c traces the
 * result. */
int kshim_exec_probe(const ds45_shim_kernel_t *k, uint64_t *out);
#endif

#endif /* DS45_KSHIM_H */
