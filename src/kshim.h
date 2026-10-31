/*
 * DS45PAD - install the DS3-route kernel shim on a fingerprinted kernel.
 *
 * On a kernel that matches a table entry, kshim_install() places the
 * freestanding shim blob (src/shim_blob.h) in a fresh kernel allocation and
 * points the DS3 driver's Bluetooth start-up and input slots at it, after
 * redirecting the product-ID checks to the blob's acceptance helper. Every
 * target is verified against its expected value first, every write is read
 * back, and any failure undoes everything already written so the kernel image
 * is left exactly as it was found. It is entirely RAM-only and runs only once,
 * at load, on the VSH side.
 *
 * The one firmware row and its F1-F6 addresses live in kshim_table.c; the
 * transactional engine is src/kpatch.h and the bounded LV2 access is
 * src/lv2.h. See docs/.../2026-10-30-ds3-route-facts.md (F1-F6) and the design
 * spec's "Install" section.
 *
 * Licensed under GPLv2 - see LICENSE.
 */
#ifndef DS45_KSHIM_H
#define DS45_KSHIM_H

#include <stdint.h>
#include "kpatch.h"

/* One firmware. The addresses are the fact sheet's F1-F6 values; the
 * fingerprint is the same (fp_addr, fp_hash) scheme ds45_kernel_t uses, so a
 * row is only ever applied to the kernel it was researched against. */
typedef struct {
	const char *name;
	uint64_t fp_addr, fp_hash;              /* same fingerprint scheme as ds45_kernel_t */
	uint64_t syscall_table; uint32_t spare_syscall; uint64_t spare_slot_expect;
	uint64_t alloc_desc; uint64_t alloc_pool;
	uint64_t flush_desc;                    /* 0 if F5 says the poke is coherent */
	uint64_t startup_slot, startup_expect;  /* F1 */
	uint64_t input_slot, input_expect;      /* F2 */
	uint64_t send_entry, send_toc;          /* F3/P4: BT-HID transfer routine h116ef0 as { entry, toc }; entry 0 = none */
	ds45_kpatch_t pid_patches[2];           /* F4, value of BRANCH = 0 -> shim_pid_ok */
} ds45_shim_kernel_t;
extern const ds45_shim_kernel_t ds45_shim_table[];
extern const int ds45_shim_table_count;

typedef enum { KSHIM_OFF = 0, KSHIM_INSTALLED, KSHIM_ALREADY, KSHIM_NO_ENTRY, KSHIM_FAILED } kshim_result_t;

/* Install over the built-in ds45_shim_table.
 *   KSHIM_NO_ENTRY  no fingerprint matched; nothing read-modified, nothing written.
 *   KSHIM_ALREADY   the redirects already point at a blob carrying DS45_SHIM_MAGIC;
 *                   the existing install is adopted and nothing is written.
 *   KSHIM_INSTALLED the shim is now installed.
 *   KSHIM_FAILED    a target was not as expected, an allocation was unusable, or a
 *                   write did not read back; everything already written is undone and
 *                   the kernel image is byte-identical to before.
 * Debug builds trace the failing step. */
kshim_result_t kshim_install(void);

/* The same over a caller-supplied table (tab may be NULL, n may be 0); this is
 * what the host tests drive with a synthetic row. kshim_install() is exactly
 * kshim_install_from(ds45_shim_table, ds45_shim_table_count). */
kshim_result_t kshim_install_from(const ds45_shim_kernel_t *tab, int n);

/* Switch the redirects back and revert the product-ID patches (a fresh install
 * restores both from its journal; an adopted one restores the redirects only,
 * the pre-patch product-ID bytes from a prior load not being recoverable). The
 * blob stays allocated, since a driver thread may still be executing inside it.
 * A no-op when nothing is installed. */
void kshim_remove(void);

/* Record, in the installed shim's data block, that the Bluetooth connection
 * with this handle (the byte the F1/F2 hooks see: drv+0xAC / xport+8) is an
 * enabled DualSense, and the connection id the kernel's transfer routine takes
 * for it. The shim's start-up sends its feature-0x05 GET to exactly that id
 * (note P1); it passes the value through untouched and cannot derive it from the
 * hook arguments, which carry only the handle (fact sheet F1), so it must be
 * captured here, when the connection comes up.
 *
 * A handle already recorded is updated in place; otherwise the first free slot
 * is used. conn_id == 0 forgets the handle (its slot stops matching, so a
 * DualShock 3 that later reuses the handle takes the original path), and
 * forgetting a handle that is not recorded writes nothing. A slot becomes
 * visible to the shim only when its state byte is set, which is written last,
 * and is hidden first when forgotten, so the shim never sees a half-written
 * connection. Every write is read back. It read-modify-writes the state qword,
 * so calls must not run concurrently; the shim only ever reads the block.
 *
 * Returns 0 once done, or -1 if nothing is installed, every slot is taken by
 * another handle, or a write did not stick. Nothing calls this yet: where the
 * composite id is read from at connect time is the open residual R3/U1. */
int kshim_set_connection(uint8_t handle, uint64_t conn_id);

#endif /* DS45_KSHIM_H */
