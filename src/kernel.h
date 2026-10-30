/*
 * DS45PAD - where a given kernel keeps its Bluetooth HID report pointer.
 *
 * A record names the slot (an LV2 address holding a pointer) and the offset
 * of the report inside the object it points to. Records are only trusted
 * when a 32-byte fingerprint of the running kernel matches, so a record for
 * one firmware is never applied to another. Fingerprints are FNV-1a 64
 * hashes; no kernel bytes are stored anywhere.
 */
#ifndef DS45_KERNEL_H
#define DS45_KERNEL_H

#include <stdint.h>
#include <cell/pad.h>   /* CellPadData, needed by ds45pad.h */
#include "ds45pad.h"

typedef struct {
	const char *name;
	uint64_t    fp_addr;     /* 0 = no fingerprint (legacy entry) */
	uint64_t    fp_hash;     /* FNV-1a 64 of the 32 bytes at fp_addr */
	uint64_t    slot_addr;
	uint32_t    report_off;
} ds45_kernel_t;

typedef enum {
	DS45_KMATCH_NONE = 0, DS45_KMATCH_TABLE, DS45_KMATCH_CACHE,
	DS45_KMATCH_SCAN, DS45_KMATCH_LEGACY
} ds45_kmatch_t;

#define DS45_FP_LEN 32
#define DS45_KERNEL_CACHE_FILE DS45_DIR "/kernel_cache.txt"

/* 1 if the DS45_FP_LEN bytes at fp_addr hash to fp_hash. 0 for fp_addr == 0,
 * and for an unaligned/out-of-window fp_addr (no peek is issued). */
int  kernel_fingerprint_matches(uint64_t fp_addr, uint64_t fp_hash);

/* First entry with a fingerprint (fp_addr != 0) that matches the running
 * kernel, or NULL. Legacy entries are never returned from here. */
const ds45_kernel_t *kernel_match_table(const ds45_kernel_t *tab, int n);

/* Cache file: one line "DS45K1 <fp_addr> <fp_hash> <slot_addr> <report_off>\n",
 * values 0x-prefixed lowercase hex. Returns 0 only if the line parses AND the
 * fingerprint re-matches the running kernel; out->name is "cached". *out is
 * untouched on failure. */
int  kernel_cache_load(ds45_kernel_t *out);
/* -1 if k has no fingerprint (fp_addr == 0) or the write fails. */
int  kernel_cache_store(const ds45_kernel_t *k);
void kernel_cache_delete(void);

extern const ds45_kernel_t ds45_kernel_table[];
extern const int           ds45_kernel_table_count;

/* ------------------------------------------------------------------------ */
/* Load-time signature scan (read-only, bounded to the LV2 window)           */
/* ------------------------------------------------------------------------ */

typedef struct { uint32_t mask, value; } ds45_insn_match_t;
typedef struct {
	const ds45_insn_match_t *insns;   /* consecutive instruction words */
	int count;
	int toc_ld_index;      /* insn `ld rT, DS(r2)`: displacement = (int16_t)(word & 0xFFFC) */
	int report_off_index;  /* insn whose (int16_t)(word & 0xFFFF) is the report offset */
} ds45_scan_pattern_t;
#define DS45_INSN_LD_FROM_R2 { 0xFC1F0003u, 0xE8020000u }  /* ld any, ds(r2) */
#define DS45_INSN_STDU_R1    { 0xFFFF0003u, 0xF8210001u }  /* stdu r1, ds(r1) */

#define DS45_SCAN_MAX_INSNS      16     /* longer patterns are rejected */
#define DS45_SCAN_MAX_HITS       8      /* more pattern hits = ambiguous */
#define DS45_SCAN_PROLOGUE_RANGE 0x800  /* bytes searched back for the stdu */

/* Walks [DS45_LV2_BASE, DS45_LV2_BOUND) once for the pattern, then once more
 * for the function descriptors of the hits. For each hit: function start =
 * the nearest stdu r1 at or before insns[0], at most
 * DS45_SCAN_PROLOGUE_RANGE bytes back; TOC = second qword of the first
 * 8-aligned pair { start, toc } with toc inside the window. Result:
 *   fp_addr    = address of insns[toc_ld_index], rounded down to 8
 *   fp_hash    = FNV-1a 64 of the DS45_FP_LEN bytes at fp_addr
 *   slot_addr  = TOC + displacement
 *   report_off = immediate of insns[report_off_index]
 * A hit is dropped if it has no prologue or descriptor, a negative report
 * offset, an unaligned or out-of-window slot, or a fingerprint leaving the
 * window. Returns 0 (out->name = "scan") only if the remaining hits agree on
 * exactly one (slot_addr, report_off); otherwise -1 and *out untouched.
 * An invalid pattern (count 0 included) returns -1 without any read. */
int kernel_scan(const ds45_scan_pattern_t *pat, ds45_kernel_t *out);
extern const ds45_scan_pattern_t ds45_scan_pattern;   /* count == 0 until Task 11 */

/* ------------------------------------------------------------------------ */
/* Resolution (once at load, before the poll loop)                            */
/* ------------------------------------------------------------------------ */

/* Largest usable report offset: a positive 16-bit immediate, with room for
 * the 16 report bytes that are read. */
#define DS45_REPORT_OFF_MAX 0x7FF0u

/* 1 if a record can be used for reads: slot_addr 8-aligned inside
 * [DS45_LV2_BASE, DS45_LV2_BOUND - 8], report_off 8-aligned and at most
 * DS45_REPORT_OFF_MAX. Every source is checked with this before it becomes
 * active; a cached record that fails it is deleted. */
int kernel_record_usable(const ds45_kernel_t *k);

/* table -> cache -> scan (stores cache) -> legacy -> NONE, over the built-in
 * table and scan pattern. Sets the active record (NULL for NONE) and copies
 * it to *out when out is non-NULL; *out is untouched for NONE. */
ds45_kmatch_t kernel_resolve(ds45_kernel_t *out);
/* The same chain over a given table and pattern (tab may be NULL, pat may be
 * NULL or invalid). Legacy = first table entry with fp_addr == 0. */
ds45_kmatch_t kernel_resolve_from(const ds45_kernel_t *tab, int n,
                                  const ds45_scan_pattern_t *pat,
                                  ds45_kernel_t *out);

const ds45_kernel_t *kernel_active(void);   /* NULL until resolved, or for NONE */
ds45_kmatch_t kernel_active_source(void);   /* NONE while kernel_active() is NULL */
/* The cross-check failed: delete the cache file, active = NULL (source NONE).
 * Nothing re-resolves afterwards. */
void kernel_reject_active(void);

/* Debug builds: DS45_TRACE of PS3MAPI (syscall 8, 0x7777) GET_FW_TYPE
 * (0x0013) and GET_FW_VERSION (0x0014). Logging only: never affects
 * resolution, and a kernel without PS3MAPI just logs the error. Release
 * builds make no call at all. */
void kernel_log_fw(void);

#endif /* DS45_KERNEL_H */
