/*
 * NPP - bounded LV2 (kernel) memory reads and writes.
 *
 * Every LV2 read in the plugin goes through npp_lv2_read()/_read_bytes(),
 * every write through npp_lv2_write(). A wild read on a real console is a
 * kernel panic and a wild write is worse, so the window and the alignment are
 * enforced here, before any syscall is issued.
 */
#ifndef NPP_LV2_H
#define NPP_LV2_H

#include <stdint.h>

#define NPP_LV2_BASE  0x8000000000000000ULL
#define NPP_LV2_BOUND 0x8000000000800000ULL   /* exclusive; 8 MiB window */

/* Syscall 6 (sys_cfw_peek). Raw and unchecked: the only syscall-6 call site
 * in this module; callers should use npp_lv2_read*() instead. */
uint64_t npp_peek(uint64_t addr);

/* 0 and *out set on success; -1 (no peek issued) if addr is not 8-byte
 * aligned or the qword is not inside [BASE, BOUND - 8]. */
int npp_lv2_read(uint64_t addr, uint64_t *out);

/* Reads len bytes (len a multiple of 8, >= 0) in memory order, i.e. byte 0 is
 * the most significant byte of the first qword. The whole range is validated
 * first; on -1 nothing was peeked and out is untouched. */
int npp_lv2_read_bytes(uint64_t addr, uint8_t *out, int len);

/* Writes one qword with syscall 7 (sys_cfw_poke), then reads it back with
 * syscall 6. Returns 0 only if the read-back equals value; -1 if the poke did
 * not stick, or (no syscall issued at all) if addr is not 8-byte aligned or
 * the qword is not inside [BASE, BOUND - 8]. The only syscall-7 call site. */
int npp_lv2_write(uint64_t addr, uint64_t value);

/* FNV-1a, 64 bit. */
uint64_t npp_fnv1a64(const uint8_t *p, int n);

#endif /* NPP_LV2_H */
