/*
 * DS45PAD - install the DS3-route kernel shim on a fingerprinted kernel.
 *
 * On a kernel that matches a table entry, kshim_install() places the
 * freestanding shim blob (src/shim_blob.h) in a fresh kernel allocation, fills
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
 * written as given); a BRANCH row would be aimed at shim_pid_ok. */
typedef struct {
	const char *name;
	uint64_t fp_addr, fp_hash;
	uint64_t syscall_table; uint32_t spare_syscall; uint64_t spare_slot_expect;
	uint64_t alloc_desc; uint64_t alloc_pool;
	uint64_t flush_desc;                        /* 0 if the poke is coherent */
	uint64_t event_slot, event_expect;          /* 0x37CFF8 / 0x33A2B0 OPD */
	uint64_t event_code_addr, event_code_hash;  /* 0xCACF0 / 0x16E42A0B1EBFEFFA */
	uint64_t enable_slot, enable_expect;        /* 0x37CFB0 / 0x33A310 OPD */
	uint64_t enable_code_addr, enable_code_hash;/* 0xCB234 / 0xEF8C3F93F63DFDB3 */
	uint64_t input_slot, input_expect;          /* 0x37CE38 / 0x33A010 OPD */
	uint64_t pad_vtable;                        /* 0x800000000038CDC8 */
	uint64_t pad_mgr;                           /* 0x80000000004A1CB8 */
	const uint8_t *info_tmpl;                   /* DS45_SHIM_INFO_LEN bytes (facts note C1.5) */
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
 *   KSHIM_FAILED    a target was not as expected, an allocation was unusable, or a
 *                   write did not read back; everything already written is undone and
 *                   the kernel image is byte-identical to before.
 * The two inline product-ID words are applied only when g_cfg.shim_inline_pid is
 * set (config key SHIM_INLINE_PID). Debug builds trace the failing step. */
kshim_result_t kshim_install(void);

/* The same over a caller-supplied table (tab may be NULL, n may be 0); this is
 * what the host tests drive with a synthetic row. kshim_install() is exactly
 * kshim_install_from(ds45_shim_table, ds45_shim_table_count). */
kshim_result_t kshim_install_from(const ds45_shim_kernel_t *tab, int n);

/* Switch the three redirects back and revert the product-ID words (a fresh
 * install restores both from its journal; an adopted one restores the redirects
 * only, the pre-edit product-ID bytes from a prior load not being recoverable).
 * The blob stays allocated, since a driver thread may still be executing inside
 * it. A no-op when nothing is installed. */
void kshim_remove(void);

#endif /* DS45_KSHIM_H */
