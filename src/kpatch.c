/*
 * NPP - verified, journalled kernel patches. See kpatch.h.
 */

#include <stddef.h>

#include "kpatch.h"
#include "lv2.h"

#define BRANCH_MIN (-0x2000000LL)   /* -32 MiB */
#define BRANCH_MAX ( 0x1FFFFFCLL)   /* +32 MiB - 4 */

/* `b` and `bl` differ only in the LK bit (bit 0), so the range and alignment
 * rules live here once and both public encoders go through it. */
static int encode_branch(uint64_t from, uint64_t to, uint32_t lk, uint32_t *word)
{
	int64_t disp;

	if (!word)
		return -1;

	/* Unsigned subtraction wraps, so a branch backwards is a negative disp. */
	disp = (int64_t)(to - from);
	if ((disp & 3) || disp < BRANCH_MIN || disp > BRANCH_MAX)
		return -1;

	*word = 0x48000000u | ((uint32_t)disp & 0x03FFFFFCu) | lk;
	return 0;
}

int kpatch_branch_word(uint64_t from, uint64_t to, uint32_t *word)
{
	return encode_branch(from, to, 0, word);
}

int kpatch_branchl_word(uint64_t from, uint64_t to, uint32_t *word)
{
	return encode_branch(from, to, 1, word);
}

/* FNV-1a 64 over one qword, bytes in memory order (as npp_lv2_read_bytes). */
static uint64_t qword_hash(uint64_t q)
{
	uint8_t b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (uint8_t)(q >> (56 - 8 * i));

	return npp_fnv1a64(b, 8);
}

/* The 32-bit instruction a WORD, BRANCH or BRANCHL patch installs. */
static int patch_word(const npp_kpatch_t *p, uint32_t *w)
{
	if (p->kind == NPP_KPATCH_WORD) {
		if (p->value > 0xFFFFFFFFULL)
			return -1;
		*w = (uint32_t)p->value;
		return 0;
	}

	if (p->kind == NPP_KPATCH_BRANCHL)
		return kpatch_branchl_word(p->addr, p->value, w);

	return kpatch_branch_word(p->addr, p->value, w);
}

/* The qword the patch leaves behind, given the qword it finds. */
static int patch_qword(const npp_kpatch_t *p, uint64_t cur, uint64_t *out)
{
	uint32_t w;

	if (p->kind == NPP_KPATCH_PTR) {
		*out = p->value;
		return 0;
	}

	if (patch_word(p, &w))
		return -1;

	if ((p->addr & 4) == 0)
		*out = ((uint64_t)w << 32) | (cur & 0xFFFFFFFFULL);
	else
		*out = (cur & 0xFFFFFFFF00000000ULL) | w;

	return 0;
}

/* 0 if the patch is well formed and its target holds what it expects. */
static int check_patch(const npp_kpatch_t *p)
{
	uint64_t cur, unused;

	switch (p->kind) {
	case NPP_KPATCH_PTR:
		/* npp_lv2_read() rejects a slot that is not 8-aligned. */
		if (npp_lv2_read(p->addr, &cur))
			return -1;
		return cur == p->expect ? 0 : -1;

	case NPP_KPATCH_WORD:
	case NPP_KPATCH_BRANCH:
	case NPP_KPATCH_BRANCHL:
		if (p->addr & 3)
			return -1;
		if (npp_lv2_read(p->addr & ~7ULL, &cur))
			return -1;
		if (qword_hash(cur) != p->expect)
			return -1;
		/* Refuse a value that could not be encoded before anything is written. */
		return patch_qword(p, cur, &unused);

	default:
		return -1;
	}
}

int kpatch_verify(const npp_kpatch_t *p, int n)
{
	int i;

	if (n < 0 || n > NPP_KPATCH_MAX || (n > 0 && !p))
		return -1;

	for (i = 0; i < n; i++)
		if (check_patch(&p[i]))
			return i + 1;

	return 0;
}

void kpatch_revert(npp_kjournal_t *j)
{
	int i;

	if (!j)
		return;

	/* A damaged count must not index past the arrays. */
	if (j->n > NPP_KPATCH_MAX)
		j->n = NPP_KPATCH_MAX;

	/* Last first, so a qword that was patched twice ends up as it began. A
	 * write that does not stick is skipped over rather than abandoning the
	 * entries still to undo. */
	for (i = j->n - 1; i >= 0; i--)
		(void)npp_lv2_write(j->qaddr[i], j->orig[i]);

	j->n = 0;
}

int kpatch_apply(const npp_kpatch_t *p, int n, npp_kjournal_t *j)
{
	int i, r;

	if (!j || n < 0 || n > NPP_KPATCH_MAX || (n > 0 && !p))
		return -1;

	j->n = 0;

	r = kpatch_verify(p, n);
	if (r)
		return r;

	for (i = 0; i < n; i++) {
		uint64_t qaddr = p[i].addr & ~7ULL;
		uint64_t cur, next;

		/* Re-read: an earlier patch may have changed this qword. */
		if (npp_lv2_read(qaddr, &cur) || patch_qword(&p[i], cur, &next)) {
			kpatch_revert(j);
			return i + 1;
		}

		/* Journal first. A write that fails its read-back may still have
		 * changed the qword, so it has to be undone too. */
		j->qaddr[j->n] = qaddr;
		j->orig[j->n] = cur;
		j->n++;

		if (npp_lv2_write(qaddr, next)) {
			kpatch_revert(j);
			return i + 1;
		}
	}

	return 0;
}
