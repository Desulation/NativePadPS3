/*
 * DS45PAD - verified, journalled kernel patches.
 *
 * A patch list is checked against the memory it is about to change, every
 * target before the first write. The writes then go in order through
 * ds45_lv2_write() (which reads each one back); the original qword is
 * journalled before each write, and any failure undoes everything already
 * written, last first. Either the whole list is applied or the image is left
 * as it was found.
 */
#ifndef DS45_KPATCH_H
#define DS45_KPATCH_H

#include <stdint.h>

typedef enum {
	DS45_KPATCH_PTR = 1,
	DS45_KPATCH_WORD,
	DS45_KPATCH_BRANCH,
	DS45_KPATCH_BRANCHL
} ds45_kpatch_kind_t;

typedef struct {
	ds45_kpatch_kind_t kind;
	uint64_t addr;      /* PTR: 8-aligned slot; WORD/BRANCH/BRANCHL: 4-aligned
	                     * instruction */
	uint64_t expect;    /* PTR: exact current value; WORD/BRANCH/BRANCHL: FNV-1a 64
	                     * of the 8-aligned qword holding the instruction, hashed
	                     * over its 8 bytes in memory order (byte 0 = most
	                     * significant) */
	uint64_t value;     /* PTR: new pointer; WORD: new 32-bit instruction;
	                     * BRANCH: absolute branch target (encoded as `b target`);
	                     * BRANCHL: absolute call target (encoded as `bl target`) */
} ds45_kpatch_t;

/* One install transaction grows to 10 entries (the output patches included),
 * so 12 leaves some headroom. The journal arrays below follow it. */
#define DS45_KPATCH_MAX 12

/* One entry per written qword, in write order. A WORD/BRANCH/BRANCHL replaces
 * the high half of its qword when (addr & 4) == 0 and the low half otherwise, so
 * qaddr is addr rounded down to 8 and orig is the whole qword before the write. */
typedef struct {
	int      n;
	uint64_t qaddr[DS45_KPATCH_MAX];
	uint64_t orig[DS45_KPATCH_MAX];
} ds45_kjournal_t;

/* Read-only. 0 = every target is as expected and every new value is
 * encodable; otherwise the 1-based index of the first patch that is not
 * (bad kind, alignment or window, expected value or hash differs, WORD value
 * wider than 32 bits, BRANCH/BRANCHL out of range). -1 if n < 0, n > DS45_KPATCH_MAX or
 * p is NULL with n > 0; then nothing was read. Each patch is checked against
 * the memory as it is now, so two patches that share a qword are both checked
 * against its original value. */
int kpatch_verify(const ds45_kpatch_t *p, int n);

/* Verifies all patches, then writes them in order, journalling each original
 * qword in j. Returns 0 on success (j->n == n). On any failure (verify, or a
 * write that does not read back) everything written is reverted, j->n is 0,
 * and the 1-based index of the failing patch is returned; a verify failure
 * writes nothing. -1, with no reads, for n < 0, n > DS45_KPATCH_MAX, a NULL j
 * or p NULL with n > 0. */
int kpatch_apply(const ds45_kpatch_t *p, int n, ds45_kjournal_t *j);

/* Writes the journalled originals back, last first, each read back, then sets
 * j->n = 0. A failed read-back does not stop the entries after it. */
void kpatch_revert(ds45_kjournal_t *j);

/* Encodes the unconditional relative branch `b to` placed at `from`
 * (primary opcode 18, AA = LK = 0) into *word. 0 on success; -1 and *word
 * untouched if to - from is not a multiple of 4 or is outside the 26-bit
 * signed range, -32 MiB .. +32 MiB - 4. */
int kpatch_branch_word(uint64_t from, uint64_t to, uint32_t *word);

/* The same for the call `bl to` (primary opcode 18, AA = 0, LK = 1): identical
 * contract and range to kpatch_branch_word, only the LK bit differs. */
int kpatch_branchl_word(uint64_t from, uint64_t to, uint32_t *word);

#endif /* DS45_KPATCH_H */
