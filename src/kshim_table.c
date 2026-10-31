/*
 * DS45PAD - the one firmware row the DS3-route shim installs on. See kshim.h.
 *
 * Every address here is a fact-sheet value for DEX 4.92 (Evilnat), transcribed
 * verbatim from docs/.../2026-10-30-ds3-route-facts.md; none is rounded or
 * invented. The F-numbers below name the section each value comes from. The
 * fingerprint (fp_addr, fp_hash) is the DEX 4.92 one from kernel.c's
 * ds45_kernel_table, reused so a row is only ever applied to the kernel it was
 * researched against.
 */

#include "kpatch.h"
#include "kshim.h"

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

		/* F6 - the LV2 allocator the driver itself uses (OPD 0x330998, entry
		 * 0x681F4, kernel TOC at +8) and its pool 0x22; pool-0x22 objects land
		 * in the 0x6xxxxx heap, within a 26-bit branch of both F4 sites. */
		0x8000000000330998ULL, 0x22ULL,

		/* F5 - the kernel's own dcbst/sync/icbi/isync range flush (entry
		 * 0x29B8, no OPD): the CFW poke is NOT i-cache coherent, so freshly
		 * written blob code must be flushed. Non-zero => flush is required. */
		0x80000000000029B8ULL,

		/* F1 - Bluetooth Sony-pad start-up slot (slot 7 of the per-transport
		 * descriptor table at 0x37CFB0) and the OPD it currently holds. */
		0x800000000037CFE8ULL, 0x800000000033A208ULL,

		/* F2 - Bluetooth transport input read (method 4 of vtable 0x37CE18,
		 * vtable+0x20) and the OPD it currently holds. */
		0x800000000037CE38ULL, 0x800000000033A010ULL,

		/* F3/P4 - the kernel's Bluetooth HID transfer routine, the one the DS3
		 * driver itself sends and drains reports with (note
		 * 2026-10-31-ds3-route-get-primitive.md, P1): entry 0x116EF0, called as
		 * int h116ef0(conn_id, dir, opclass, buf, len). The DS3 driver has NO
		 * GET_REPORT helper (0x102750/OPD 0x3408C8 is SET-only and hard-codes
		 * the 0x50 header), so the shim builds the {0x43, 0x05} PDU itself and
		 * sends it through this routine. The routine is OPD-less, like the F5
		 * flush, so the row gives { entry, toc } and the installer builds the
		 * descriptor in the data block; the toc is the one kernel TOC every OPD
		 * on this firmware carries (fact sheet header, 0x3759C0). That the send
		 * alone flips the pad to report 0x31 is the on-console residual R2. */
		0x8000000000116EF0ULL, 0x80000000003759C0ULL,

		/* F4 - product-ID acceptance. Two BRANCH redirects to the blob's
		 * shim_pid_ok (OFF_PID_OK), each verified by the FNV hash of the
		 * 8-aligned qword holding its patch instruction. BRANCH value is left 0
		 * here and filled with the blob load base + OFF_PID_OK at install time
		 * (the target is not known until the blob is allocated).
		 *   [0] inline SDP site: the 0x03D5 compare at 0x11B5E4, qword 0x11B5E0.
		 *   [1] helper 0x29E2B4 first instruction, qword 0x29E2B0. */
		{
			{ DS45_KPATCH_BRANCH, 0x800000000011B5E4ULL, 0xE8ADD0EDF3B2B3C1ULL, 0ULL },
			{ DS45_KPATCH_BRANCH, 0x800000000029E2B4ULL, 0xF1B8B3EFF22A78FCULL, 0ULL },
		},
	},
};
const int ds45_shim_table_count =
	(int)(sizeof(ds45_shim_table) / sizeof(ds45_shim_table[0]));
