/*
 * DS45PAD - the one firmware row the DS3-route shim installs on. See kshim.h.
 *
 * Every value here is a researched DEX 4.92 (Evilnat) value, transcribed
 * verbatim from the fact notes; none is rounded or invented:
 *   - docs/superpowers/notes/2026-10-30-ds3-route-facts.md (F2 input slot, F4
 *     inline sites, F5 flush, F6 allocator and spare syscall);
 *   - docs/superpowers/notes/2026-11-02-ds3-route-startup-completion-facts.md
 *     (C1.5 info template, C3 enable driver, C4 event/enable/helper rows).
 * The fingerprint (fp_addr, fp_hash) is the DEX 4.92 one from kernel.c's
 * ds45_kernel_table, reused so a row is only ever applied to the kernel it was
 * researched against.
 *
 * Product-ID acceptance in stage 1 is pure kernel word edits (WORD rows), the
 * variant proven hang-free on the console: nothing branches into the blob on the
 * connect path. A BRANCH row is only valid at the FIRST INSTRUCTION of a
 * function whose calling convention the blob's target shares (note
 * 2026-10-31-ds3-route-pid-patch-fix.md); stage 1b may add one.
 */

#include "kpatch.h"
#include "kshim.h"
#include "../shim/shim_abi.h"   /* DS45_SHIM_INFO_LEN */

/* DS3 info block, pad+0x80..+0xD5 (0x56 bytes), Bluetooth variant (facts note
 * C1.5). Index = pad offset - 0x80. The per-pad identity at index 0x08..0x0C,
 * 0x0E, 0x0F is zero here: the shim fills it per pad from its completion count.
 * The kernel's check 0x2A3AD8 passes with these values (reg ver 5, fw 0x350,
 * variant 0, +0x8E != 0xD, +0xD5 == 0). */
static const uint8_t DS3_INFO_TEMPLATE[DS45_SHIM_INFO_LEN] = {
	0x00, 0x05, 0x03, 0x50, 0x00, 0x00, 0x00, 0x00,  /* +0x80 reg ver 0x0005, fw 0x0350, variant 0 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0x88 identity: PER PAD */
	0x00, 0x00, 0x01, 0x8A, 0x01, 0x00, 0x00, 0x00,  /* +0x90 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0x98 never read, never written */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xA0 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xA8 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xB0 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFA,  /* +0xB8 (+0xBE: calibration starts) */
	0x01, 0x8E, 0x02, 0x00, 0x01, 0x91, 0x02, 0x00,  /* +0xC0 */
	0x01, 0x8D, 0x01, 0xE6, 0x00, 0x00, 0x02, 0x6D,  /* +0xC8 */
	0x02, 0x70, 0x01, 0x01, 0x01, 0x00               /* +0xD0 (+0xD2..+0xD5: flags 1,1,1,0) */
};

const ds45_shim_kernel_t ds45_shim_table[] = {
	{
		"DEX 4.92",
		/* fingerprint: the first `ld r3,-0x35E0(r2)`, same bytes kernel.c's
		 * DEX 4.92 row fingerprints. */
		0x80000000000F3380ULL, 0xDF192C9D78181BF7ULL,

		/* F6 - syscall table base, a spare (ni-stub) syscall and its current
		 * slot value. Slot = base + 149*8 = 0x800000000038A990, which reads as
		 * the not-implemented stub OPD 0x80000000003621B0 until we borrow it. */
		0x800000000038A4E8ULL, 149u, 0x80000000003621B0ULL,

		/* F6 - the LV2 allocator's OPD (0x330998, entry 0x681F4, kernel TOC at
		 * +8). No allocation is done any more (design Amendment 2); only its +8
		 * is read, for the kernel TOC the flush and probe OPDs need. */
		0x8000000000330998ULL,

		/* Amendment 2 - the fixed executable region the blob is placed in: the
		 * CFW payload's free zero run 0x57C000..0x580000 (0x4338 bytes, 8-aligned,
		 * identical and unused across every dump sampled). The blob 0x1740 + the
		 * 0x10 flush OPD end well before exec_bound. */
		0x800000000057C000ULL, 0x8000000000580000ULL,

		/* F5 - the kernel's own dcbst/sync/icbi/isync range flush (entry
		 * 0x29B8, no OPD): the CFW poke is NOT i-cache coherent, so freshly
		 * written blob code must be flushed. Non-zero => flush is required. */
		0x80000000000029B8ULL,

		/* C4 - the start-up driver's event-handler slot (vtable 0x37CFC8) and
		 * the OPD it holds; that OPD's code 0xCACF0 and its first qword's FNV. */
		0x800000000037CFF8ULL, 0x800000000033A2B0ULL,
		0x80000000000CACF0ULL, 0x16E42A0B1EBFEFFAULL,

		/* C3/C4 - the enable (0xF4) driver's event-handler slot (vtable
		 * 0x37CF80) and the OPD it holds; that OPD's code 0xCB234 and the FNV
		 * of the 8-aligned qword 0xCB230 holding it (hash_ok hashes addr & ~7;
		 * NOT the 8 bytes from 0xCB234, which hash to 0xEF8C3F93F63DFDB3). */
		0x800000000037CFB0ULL, 0x800000000033A310ULL,
		0x80000000000CB234ULL, 0xB52798EFCFE165A8ULL,

		/* F2 - Bluetooth transport input read (method 4 of vtable 0x37CE18,
		 * vtable+0x20) and the OPD it currently holds. */
		0x800000000037CE38ULL, 0x800000000033A010ULL,

		/* C1/C2 - the Sony pad class and the global holding the pad manager. */
		0x800000000038CDC8ULL,
		0x80000000004A1CB8ULL,

		DS3_INFO_TEMPLATE,

		/* C4 - helper 0x29E2B4: its `xori 0x042F` at 0x29E2CC becomes
		 * `xori 0x0CE6`, so the helper accepts the DualSense where it accepted
		 * the Move navigation controller. Verified by the FNV of the qword
		 * 0x29E2C8 holding it. */
		{
			{ DS45_KPATCH_WORD, 0x800000000029E2CCULL, 0x8D1047F832C76096ULL, 0x68800CE6ULL },
		},

		/* F4 - the inline compare at the connection path, applied only with
		 * SHIM_INLINE_PID = 1 (see 2026-10-31-ds3-route-pid-patch-fix.md):
		 *   [0] qword 0x11B5E0: the third compare (0x03D5) becomes
		 *       cmpwi cr7,r0,0x0CE6; the bne cr7,0x11B534 after it is kept.
		 *   [1] qword 0x11B5E8: the fall-through "b 0x11B564" becomes
		 *       "b 0x11B234" (known Sony pad, skip SDP) - reached only for PID
		 *       0x0CE6. */
		{
			{ DS45_KPATCH_WORD, 0x800000000011B5E4ULL, 0xE8ADD0EDF3B2B3C1ULL, 0x2F800CE6ULL },
			{ DS45_KPATCH_WORD, 0x800000000011B5ECULL, 0xF86140C9C3D7FB8DULL, 0x4BFFFC48ULL },
		},
	},
};
const int ds45_shim_table_count =
	(int)(sizeof(ds45_shim_table) / sizeof(ds45_shim_table[0]));
