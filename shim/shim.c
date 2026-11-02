/*
 * DS45PAD kernel shim - decision logic (start-up and enable completion, pad
 * identity, input, product ID).
 *
 * Built twice, exactly like ds3_report.c: natively by the host tests, and
 * freestanding into the shim blob that runs inside the PS3 kernel's DS3 driver.
 * So no SDK, no libc, no static data (nothing that would leave a .rodata/TOC
 * entry the packer cannot relocate). Each function takes the data block and the
 * kernel objects explicitly, so the kernel-specific work stays in the thin glue
 * in entry.c and these bodies stay pure and testable.
 *
 * Kernel objects are reached in two ways only: pointers (the driver's pad and
 * info pointers, the manager pointer, the manager's object pointers, the pad's
 * transport pointer, the pad's class) are read as native u64, which is what they
 * are on the PPU; everything this code writes, and the VID/PID it compares, is
 * a single byte at a time in the kernel's big-endian order, so no store depends
 * on the host's byte order. No sends, no locks, no allocation, no calls into the
 * kernel, and every loop is bounded by a constant.
 *
 * The interfaces are shim.h; the object layouts are the DEX 4.92 ones of the
 * facts note docs/superpowers/notes/2026-11-02-ds3-route-startup-completion-
 * facts.md (C1-C3). Licensed under GPLv2 - see LICENSE.
 */

#include "shim.h"
#include "ds3_report.h"

/* Sony USB identity. */
#define VID_SONY        0x054C
#define PID_DS3         0x0268   /* DualShock 3            */
#define PID_MOVE_NAV    0x042F   /* PlayStation Move navigation */
#define PID_DUALSENSE   0x0CE6   /* DualSense              */

/* DualSense Bluetooth full report id (the same value as DS_IN_BT; redefined
 * here because the freestanding shim must not pull in ds45_state.h). */
#define DS_BT_INPUT_ID  0x31

/* The fixed identity bytes after the counter: info+0x0C..+0x0F. 0xD5 keeps the
 * check's alternate branch (+0x8C == 0x18 with +0x8E == 0x000D) out of reach,
 * and "DS" at +0x0E is never 0x000D. */
#define SHIM_ID_TAG0    0xD5
#define SHIM_ID_TAG1    0x00
#define SHIM_ID_TAG2    0x44     /* 'D' */
#define SHIM_ID_TAG3    0x53     /* 'S' */

/* A u64 that may alias any object. The kernel objects are reached through byte
 * pointers (and are byte buffers in the host tests), and the blob is built -O2
 * without -fno-strict-aliasing, so the loads below go through this type: the
 * compiler may then not assume they cannot see the byte stores made through
 * uint8_t (GCC/clang attribute; no header, freestanding-safe). */
typedef uint64_t __attribute__((may_alias)) shim_u64_alias_t;

/* A native 64-bit load of the kernel pointer at p (8-aligned in every object
 * this reads). */
static inline uint64_t ld64(const uint8_t *p)
{
	return *(const shim_u64_alias_t *)(const void *)p;
}

static inline uint8_t *ld_ptr(const uint8_t *p)
{
	return (uint8_t *)(uintptr_t)ld64(p);
}

int shim_is_dualsense(uint16_t vid, uint16_t pid)
{
	return vid == VID_SONY && pid == PID_DUALSENSE;
}

/* Product-ID helper. A leaf of immediate comparisons only: no global and no TOC
 * access, so the kernel can branch to it with its own r2 (tools/shim_pack.py
 * enforces this over this function's code range). Keeps 0x0268 and 0x042F, adds
 * 0x0CE6. Stage 1 accepts the DualSense with kernel word edits instead; this
 * stays for stage 1b. */
int shim_pid_ok(uint32_t vid, uint32_t pid)
{
	return vid == VID_SONY &&
	       (pid == PID_DS3 || pid == PID_MOVE_NAV || pid == PID_DUALSENSE);
}

int shim_pad_is_dualsense(const ds45_shim_data_t *d, const uint8_t *pad)
{
	if (!d || !pad || d->pad_vtable == 0)
		return 0;
	if (ld64(pad) != d->pad_vtable)
		return 0;
	return pad[SHIM_PAD_ID + 0] == (uint8_t)(VID_SONY >> 8) &&
	       pad[SHIM_PAD_ID + 1] == (uint8_t)VID_SONY &&
	       pad[SHIM_PAD_ID + 2] == (uint8_t)(PID_DUALSENSE >> 8) &&
	       pad[SHIM_PAD_ID + 3] == (uint8_t)PID_DUALSENSE;
}

/* Status 0 in the u32 at drv+0x50, byte by byte. */
static inline void put_status_ok(uint8_t *drv)
{
	drv[SHIM_DRV_STATUS + 0] = 0;
	drv[SHIM_DRV_STATUS + 1] = 0;
	drv[SHIM_DRV_STATUS + 2] = 0;
	drv[SHIM_DRV_STATUS + 3] = 0;
}

int shim_event_complete(ds45_shim_data_t *d, uint8_t *drv)
{
	uint8_t *pad, *info;
	uint32_t n;
	int i;

	if (!d || !drv || !d->enabled)
		return 0;
	pad = ld_ptr(drv + SHIM_DRV_PAD);
	if (!shim_pad_is_dualsense(d, pad))
		return 0;
	/* A later event for a driver already done is the original's business. */
	if (drv[SHIM_DRV_SUBSTATE] == SHIM_DRV_DONE)
		return 0;
	/* The info pointer is trusted only as the validated pad's own info block
	 * (pad+0x80): anything else - another object, another offset, NULL - would
	 * have the 0x56-byte copy land somewhere unvalidated, so leave it all to
	 * the original. */
	info = ld_ptr(drv + SHIM_DRV_INFO);
	if (info != pad + SHIM_PAD_INFO)
		return 0;

	/* 1. the DS3 info block (identity bytes zero in the template) */
	for (i = 0; i < DS45_SHIM_INFO_LEN; i++)
		info[i] = d->info_tmpl[i];

	/* 2. this pad's identity: the completion count, then the fixed tag. The
	 * start-up framework runs driver events one at a time, so a plain
	 * increment numbers every DualSense of a boot uniquely. stat_completed is
	 * therefore not just a debug counter: nothing may ever reset it (see
	 * shim_abi.h), or two pads of one boot could get the same identity. */
	n = ++d->stat_completed;
	info[SHIM_ID_OFF + 0] = (uint8_t)(n >> 24);
	info[SHIM_ID_OFF + 1] = (uint8_t)(n >> 16);
	info[SHIM_ID_OFF + 2] = (uint8_t)(n >> 8);
	info[SHIM_ID_OFF + 3] = (uint8_t)n;
	info[SHIM_ID_OFF + 4] = SHIM_ID_TAG0;
	info[SHIM_ID_OFF + 5] = SHIM_ID_TAG1;
	info[SHIM_ID_OFF + 6] = SHIM_ID_TAG2;
	info[SHIM_ID_OFF + 7] = SHIM_ID_TAG3;

	/* 3. status 0 and "done": the framework then calls the ready callback with
	 * status 0 and the kernel's own version check runs on the block above. */
	put_status_ok(drv);
	drv[SHIM_DRV_SUBSTATE] = SHIM_DRV_DONE;
	return 1;
}

int shim_enable_complete(ds45_shim_data_t *d, uint8_t *drv)
{
	if (!d || !drv || !d->enabled)
		return 0;
	if (!shim_pad_is_dualsense(d, ld_ptr(drv + SHIM_DRV_PAD)))
		return 0;
	if (drv[SHIM_EN_DONE] != 0)
		return 0;

	/* Status 0 and the done flag: the framework calls the callback with status
	 * 0 and the pad manager assigns the port. */
	put_status_ok(drv);
	drv[SHIM_EN_DONE] = 1;
	d->stat_enabled++;
	return 1;
}

const uint8_t *shim_pad_from_xport(const ds45_shim_data_t *d, const void *xport)
{
	const uint8_t *mgr, *obj;
	int i;

	if (!d || !d->pad_mgr || !xport)
		return 0;
	mgr = ld_ptr((const uint8_t *)(uintptr_t)d->pad_mgr);
	if (!mgr)
		return 0;

	/* The pad being read is alive (its own input handler is the caller), so the
	 * table holds it; the other entries are only compared, never followed
	 * further than their transport pointer. */
	for (i = 0; i < SHIM_MGR_NOBJS; i++) {
		obj = ld_ptr(mgr + SHIM_MGR_OBJS + 8 * i);
		if (obj && ld64(obj + SHIM_PAD_XPORT) == (uint64_t)(uintptr_t)xport)
			return obj;
	}
	return 0;
}

int shim_input_filter(ds45_shim_data_t *d, uint16_t vid, uint16_t pid,
                      uint8_t *buf, uint16_t *len)
{
	int ok = 0;

	/* Translate only an enabled DualSense's basic 0x01 or full 0x31 report.
	 * Both translators read the whole report before they write a byte, so buf
	 * may be their own output (an in-place rewrite), and on any refusal - wrong
	 * id, too short - they write nothing and return -1, which leaves buf and
	 * *len exactly as the original read left them. */
	if (d && d->enabled && buf && len && shim_is_dualsense(vid, pid)) {
		if (buf[0] == DS_BASIC_REPORT_ID)
			ok = ds3_report_from_dualsense_basic(buf, (int)*len, &d->cfg, buf) == 0;
		else if (buf[0] == DS_BT_INPUT_ID)
			ok = ds3_report_from_dualsense(buf, (int)*len, &d->cfg, buf) == 0;
	}
	if (ok) {
		*len = DS3_REPORT_LEN;
		d->stat_translated++;
		return DS3_REPORT_LEN;
	}

	/* stat_passed counts EVERY read not translated: a DS3's reads too (entry.c
	 * scans the 16 manager entries for every plausible read, DS3 or not, and
	 * calls this with vid/pid 0 for a non-DualSense), plus a DualSense's
	 * non-translatable reports. On the console it grows at the DS3's report
	 * rate; it is not a count of DualSense failures. */
	if (d)
		d->stat_passed++;
	return len ? (int)*len : 0;
}
