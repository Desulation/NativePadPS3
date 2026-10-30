/*
 * DS45PAD - passive cross-check of the Bluetooth HID report location.
 *
 * The kernel table / cache / scan decide where the raw BT report lives on the
 * running firmware. This module confirms that decision without touching the
 * pad: it compares the raw reports the plugin decodes with what VSH's own pad
 * reads (cellPadGetData detours) see for the same physical controller. After
 * BTCHECK_MATCHES_REQUIRED agreeing samples the location is trusted (PASSED);
 * too many disagreements, or a raw buffer that keeps changing while the pad is
 * disconnected, mean it is not a live report (FAILED) and the plugin steps
 * aside.
 *
 * Pure logic: no LV2 reads, no clock (time is passed in). VSH-only; the game
 * build does not link it.
 *
 * Contract for callers:
 *  - Single instance, no locking of its own: calls to btcheck_feed_raw,
 *    btcheck_observe_vsh, reset, state and samples_seen must never overlap.
 *    The plugin serialises them with one mutex in passthru.c (VSH detours
 *    trylock and drop the sample; the plugin thread locks).
 *  - btcheck_observe_vsh needs CellPadData.len to be set (as cellPadGetData
 *    does; len == 0 means "no data this poll"). len >= 8 is required, since
 *    button[7] (DS45_BTN_ANALOG_LEFT_Y) is the highest word read; a shorter
 *    len is ignored, so a caller that leaves len unset is never counted.
 *  - src_port must be 0..7. Any other value (negative, or 8 and up) means
 *    nothing is ever observed: every VSH read is ignored.
 *  - A match must be fresh evidence. Each history entry has a sequence number
 *    assigned when it is pushed. A counted VSH change is a MATCH only if the
 *    NEWEST history entry it agrees with is newer than the entry used by the
 *    previous counted match. Agreeing only with an entry that is not newer is
 *    NEUTRAL (neither match nor mismatch); agreeing with nothing is a
 *    mismatch. So a static raw buffer (e.g. a stale neutral A1 01 report)
 *    can never reach PASSED however much the VSH side jitters: the raw side
 *    must be changing too. btcheck_reset and a disconnect while PENDING
 *    forget the previous match.
 *  - The raw history holds the last BTCHECK_RAW_HISTORY DISTINCT raw states
 *    (a sample equal to the newest entry is not pushed), so a high raw feed
 *    rate does not push an older state out from under a lagging VSH read.
 *  - Fail-safe freeze rule: a raw buffer that changes more than
 *    BTCHECK_FREEZE_US after the pad disconnected is FAILED, in PENDING and
 *    in PASSED alike (a trusted location that keeps changing with the pad
 *    gone is revoked). FAILED is terminal until btcheck_reset.
 */
#ifndef DS45_BTCHECK_H
#define DS45_BTCHECK_H

#include <stdint.h>
#include <cell/pad.h>   /* CellPadData, needed by ds45pad.h */
#include "ds45pad.h"

/* One decoded raw BT report (hat is the raw 0..8 value; d1/d2 are the
 * DS45_CTRL_* words, d1 includes DS45_CTRL_PS when ps is set). */
typedef struct {
	uint8_t  lx, ly, rx, ry, l2, r2, hat;
	uint16_t d1, d2;
	int      ps;
} ds45_btraw_t;

/* Decode the first 16 bytes of the kernel's report from two big-endian
 * doublewords. Returns -1 (and leaves *out alone) unless bytes 0-1 are A1 01,
 * the hat is <= 8 and out is non-NULL. */
int bt_report_decode(uint64_t w0, uint64_t w1, ds45_btraw_t *out);

typedef enum { BTCHECK_PENDING = 0, BTCHECK_PASSED, BTCHECK_FAILED } btcheck_state_t;

#define BTCHECK_MATCHES_REQUIRED 50
#define BTCHECK_MISMATCH_LIMIT   10
#define BTCHECK_STICK_TOLERANCE  2
#define BTCHECK_FREEZE_US        200000
#define BTCHECK_RAW_HISTORY      8

/* Start over, watching VSH reads of src_port. The only call that clears a
 * FAILED state, the VSH sample counter and the source port. */
void btcheck_reset(int src_port);

/* From the VSH pad-read detours: one call per poll. Reads on any port other
 * than the source port, and polls without data (len too short), are ignored. */
void btcheck_observe_vsh(uint32_t port, const CellPadData *d);

/* One raw sample per poll, with whether the pad is currently connected and a
 * monotonic microsecond timestamp. Returns the state after this sample. */
btcheck_state_t btcheck_feed_raw(const ds45_btraw_t *r, int port_connected, uint64_t now_us);

btcheck_state_t btcheck_state(void);

/* Counted (changed) VSH samples since btcheck_reset; keeps counting in every
 * state, FAILED included. */
int btcheck_vsh_samples_seen(void);

#endif /* DS45_BTCHECK_H */
