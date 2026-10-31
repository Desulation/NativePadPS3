# DS3-route PID patch: why the DualSense got no pad object, and the fix (DEX 4.92)

Read-only static analysis for the GPLv2 DS3-route shim, on the user's own CFW DEX 4.92
kernel. No console write, no product code. Inputs: `analysis/ff_492.json` and
`analysis/lv2_code_492.bin` (pristine kernel code, LV2 offset = file offset) plus three RAM
dumps from `tools/dumps/dex492/` (git-ignored, never committed):

| tag | file | state |
|---|---|---|
| shim dump | `shimtest/connected_lv2.bin` | shim installed and live, DualSense (type 0x01) connected, **no pad object** |
| limbo dump | `ds3route/type01_connected.bin` | no PID patch, DualSense type 0x01 connected, no pad object |
| manual dump | `ds3route/navpid_connected.bin` | hand-patched 0x042F compare immediates -> 0x0CE6, **pad object formed** |

Conventions as in the facts note: a bare `0xNNNN` is an LV2 offset (kernel address =
`0x8000000000000000` + offset); hashes are FNV-1a-64 of the 8-aligned qword, bytes in memory
order. No raw kernel bytes or listings here: mnemonics in prose, addresses, hashes, and the
shim's own patch words only.

## Verdict

1. **The suspected cause is confirmed, and it is a hang, not just a wrong answer.** The
   inline BRANCH at 0x11B5E4 enters `shim_pid_ok`, which (a) reads r3/r4 where the site holds
   the PID in r0 and the VID in r9, (b) returns a 0/1 in r3 and executes `blr` where the site
   needs a jump to one of three kernel addresses, and (c) - decisively - a plain `b` does not
   set LR, so that `blr` goes back to **0x11B528**, the return address of the last call the
   handler made. That is the VID read just above the PID block, so the block re-runs forever.
   The kernel thread running the BT event handler spins in this loop; the shim dump contains
   its register-save frames (section 2).
2. That alone explains every observation: both continuations of the block (the Sony route at
   0x11B234 and the SDP route at 0x11B564) are downstream of the spin, so neither ran - no SDP
   response buffers, no handshake byte, no pad attach, no pad object, and the start-up and
   input hooks never fired (counters 0).
3. **The helper branch at 0x29E2B4 is correct and stays.** It is the first instruction of a
   leaf with exactly `shim_pid_ok`'s ABI, and its callers `bl` it, so LR is theirs.
4. **Pad-object creation is gated by the helper**: the pad factory 0x2A128C calls 0x29E2B4
   (call at 0x2A14B0) for device types 1 and 2 and only on a non-zero answer allocates the
   0x520-byte object and runs the constructor. The inline block is the earlier BT-route gate
   (Sony pad route vs SDP route). The manual experiment relaxed both; so must the shim.
5. **Fix (Option A, recommended):** keep the helper BRANCH row; replace the inline BRANCH row by
   two WORD rows - the third compare becomes `cmpwi cr7,r0,0x0CE6` and the fall-through
   `b 0x11B564` becomes `b 0x11B234`. DS3, Move navigation and DualSense are accepted exactly as
   in the proven experiment; every other PID decides exactly as today, except 0x03D5 (Move
   motion), which now takes the generic decision instead of going straight to SDP (section 4.3).

## 1. The two sites

### 1.1 Inline site - function 0x11B098, block 0x11B5D0..0x11B5EC

0x11B098 is the BT upper-stack event handler (entered through the dispatcher 0x11AD14). Its
branch for message 0x7022 (starts 0x11B4A4) looks up the connection object for the handle (type-4
lookup 0x1125F4), and, if a device-info callback is registered (global at TOC-0x2438, callback
table at +0x60), clears a 4-byte buffer on its stack - VID at sp+0xB4, PID at sp+0xB6 - and
calls the callback with (the connection's 48-bit id, kind 2, buffer, length 4). After the last
call of the path (`bl 0x112EC8` at 0x11B524) it does:

- **VID in r9**: `lhz r9,0xB4(r1)` at 0x11B528; `cmpwi cr7,r9,0x54C` at 0x11B52C; `beq` to
  0x11B5D0 at 0x11B530 (any other VID goes to 0x11B534).
- **PID in r0**: `lhz r0,0xB6(r1)` at 0x11B5D0. Then three compare/branch pairs, all on cr7:
  PID 0x0268 -> `beq` 0x11B234 (0x11B5D4/D8); PID 0x042F -> `beq` 0x11B234 (0x11B5DC/E0);
  **0x11B5E4: `cmpwi cr7,r0,0x03D5`** (the instruction the shim replaced), 0x11B5E8:
  `bne cr7,0x11B534`, 0x11B5EC: `b 0x11B564`. So only PIDs other than 0x0268/0x042F ever reach
  0x11B5E4.
- **Exits.** 0x11B234: "known Sony pad" - `bl 0x115CA8` with the connection's id (the
  connection-continuation routine: it steps the connection's state machine and can post event
  0x1406 to the BT event ring), then leave the handler. 0x11B564: post message 0x502C (the SDP
  query, per the earlier notes; the handler for 0x502C, at 0x11B204, also ends at 0x11B234).
  0x11B534: the generic decision - connection state (+0xC) != 1 -> 0x11B234; flag bit 0x40
  (+4, set by a remote-name match) -> 0x11B234; VID != 0 -> 0x11B564; VID == 0 and PID == 0 ->
  0x11B234; otherwise 0x11B564.
- **Live registers at 0x11B5E4**: r0 = PID, r9 = VID (0x054C), r28 = connection object, r1 =
  the handler's frame, r2 = kernel TOC; LR is dead but holds 0x11B528 (below); cr7 is consumed
  only by the `bne` at 0x11B5E8. Correction to the F4 contract: **r9 is live** into 0x11B534
  (tested again at 0x11B550), not clobberable.

### 1.2 Helper - 0x29E2B4

A 64-byte leaf, no stack, no calls: **VID in r3, PID in r4, result in r3, `blr`.** It returns 1
iff VID == 0x054C and PID is 0x0268 or 0x042F (the 0x0268 test is `cmpwi cr6,r4,0x268`, the
0x042F test is `xori r0,r4,0x42F` at 0x29E2CC). It clobbers only r0, r3 and cr6/cr7. A direct
`bl`/`b` scan of the code image finds **six** callers, not three: 0x2A092C, 0x2A14B0, 0x2A18EC,
0x2A1A90, 0x2A20C8, 0x2A23A0 (the function finder's call graph missed three). Each loads VID
and PID from bytes into r3/r4, calls, then takes the low byte of r3 (`clrlwi r3,r3,24`) and
re-compares - no register or CR assumption survives the call.

### 1.3 Confirmed against the dumps

| item | pristine code | limbo dump | manual dump | shim dump |
|---|---|---|---|---|
| 0x11B5DC compare immediate | 0x042F | 0x042F | **0x0CE6** | 0x042F |
| 0x11B5E4 | `cmpwi cr7,r0,0x3D5` | same | same | **`b 0x61039C`** |
| 0x29E2B4 | `cmpwi cr7,r3,0x54C` | same | same | **`b 0x61039C`** |
| 0x29E2CC (helper) | `xori r0,r4,0x42F` | same | **`xori r0,r4,0xCE6`** | 0x42F |
| qword 0x11B5E0 hash | 0xE8ADD0EDF3B2B3C1 | same | - | 0xE72CDE264E2B5E82 |
| qword 0x29E2B0 hash | 0xF1B8B3EFF22A78FC | same | - | 0x81616B05371FB072 |

The pristine hashes equal the `kshim_table.c` `expect` values. The shim dump's blob is based at
0x610000: `shim_pid_ok` sits at 0x61039C and is, as compiled, a leaf that uses only r3, r4, r5,
cr0 and cr1, touches no TOC or stack, and ends in `blr`.

## 2. Why the inline branch breaks, and why the helper branch does not

**Mechanism.** The only way into 0x11B5D0 is the `beq` at 0x11B530, reached after
`bl 0x112EC8` at 0x11B524 (LR = 0x11B528) with no call in between. `b 0x61039C` leaves LR
alone; `shim_pid_ok` is a frameless leaf that does not touch LR, r1 or the stack; its `blr`
returns to 0x11B528. The VID/PID bytes on the stack are unchanged, so the same PID (not 0x0268
or 0x042F, VID 0x054C) reaches 0x11B5E4 again - an infinite loop of about 25 instructions (16
of them in the blob) with no call and no exit. The result in r3 is never looked at. A scan of
the whole code image finds no other branch into 0x11B5D0 or 0x11B528.

**Evidence in the shim dump (the loop's fixed point, caught by the scheduler).**

| where | value |
|---|---|
| register-save area at 0x3AF318 (r0 at +0x00, one qword per GPR) | r0 = 0x0CE6, r2 = 0x3759C0 (kernel TOC), r9 = 0x054C, r28 = 0x4B0170, r29 = 0x4B9528, r1 = 0x800000001006CAB0 |
| same frame, 0x3AF428 / 0x3AF438 / 0x3AF440 (roles by position) | two qwords = 0x11B528 (LR and saved PC) followed by an MSR-shaped value, 0x8000000000008032 |
| same frame, r3 / r4 / r5 | 0 / 0 / 1 - exactly the steady state of repeated `shim_pid_ok` calls on garbage inputs |
| context-switch frame at 0x4C5658 (roles by position) | saved PC = 0x6103A8 (inside `shim_pid_ok`), then 0x800000001006CAB0 (the same stack pointer), then LR = 0x11B528 |
| 0x391FD0 / 0x391FD8 | a further pair of 0x11B528 values |
| limbo and manual dumps | no qword equal to 0x800000000011B528 anywhere |

The GPR slot identification rests on five independent matches (r0 = the PID, r9 = the VID, r2 =
the TOC, r28 = connection-table entry 0 whose id field at +0x10 is the BD address at 0x4B0180,
r29 = the value of TOC slot -0x2438, which is what 0x11B098 loads into r29 in its prologue). So
a PPU thread was caught at the loop head with LR = PC = 0x11B528 and r0 = 0x0CE6 (a PID that has
already been through one pass), and the same stack caught inside the blob. The wedge is local
to the thread that runs the BT event handler: the BT HID mutex (0x4325D0 -> 0x51A280) is
identical to the other dumps (free), and the 2026-10-31 DEX session saw a healthy console. What
stops is BT event handling for new connections.

**Blast radius (decision block, simulated on the pristine image):**

| PID | original kernel | live shim | manual experiment | Option A |
|---|---|---|---|---|
| 0x0268 DS3 | 0x11B234 (known pad) | 0x11B234 | 0x11B234 | 0x11B234 |
| 0x042F Move navigation | 0x11B234 | 0x11B234 | 0x11B534 (generic) | 0x11B234 |
| 0x0CE6 DualSense | 0x11B534 (generic -> SDP) | **loop** | 0x11B234 | 0x11B234 |
| 0x03D5 Move motion | 0x11B564 (SDP) | **loop** | 0x11B564 | 0x11B534 (generic -> SDP) |
| 0x05C4 / 0x09CC DS4 | 0x11B534 | **loop** | 0x11B534 | 0x11B534 |

So the current build wedges the BT event thread for **any** BT pad whose registry record carries
VID 0x054C with a PID other than a DS3's or a Move navigation controller's - a DS4 included -
until reboot. It is RAM-only, so a reboot clears it, and nothing is damaged.

**The helper is fine.** `b` from the first instruction of a leaf called with `bl`: LR is the
caller's, `blr` returns to it, r3 carries the answer, and the only extra clobbers (r4, r5, cr0,
cr1) are volatile and not assumed preserved by any of the six call sites. It has not run yet on
the console only because the pad factory was never reached.

**Side results.** (a) U3 is settled for this console: pool-0x22 memory executes (a saved PC lies
inside the blob, and the loop ran through it repeatedly). (b) The F5 flush of the blob worked
(the blob executed as compiled). (c) The poke at 0x11B5E4 took effect without a dedicated flush
of the patched kernel text (a cold line); see 4.5 for hardening.

## 3. What gates pad-object creation, and what the manual patch changed

**The pad factory.** The attach-notification handler at 0x2A1600 (event id 0x1A) parses VID and
PID out of the event payload and calls the factory 0x2A128C with the device-id word (type in
bits 8-15). The factory reserves a slot, then switches on the type: type 3 builds a 0x138-byte
object, type 4 a 0xF0-byte object, **types 1 and 2 call the helper 0x29E2B4 with (VID, PID) at
0x2A14B0, and only if it returns non-zero allocate 0x520 bytes from pool 0x22 and call the pad
constructor 0x2A3ECC** (the factory is its only direct caller; it installs the pad class vtable
0x38CDC8), then store the object in the manager's table at +0x60. A zero answer builds nothing
at all and the call fails with 0x80010005 - the "limbo" in the limbo dump (a type-0x01 pad with no Sony object
and no generic object). The other five helper callers act on existing objects or arguments
(classification, port handling) and none of them reaches the constructor.

**The inline block** is upstream, in the BT connection path: it picks the Sony route (0x11B234,
skip the SDP query) or the generic route (0x11B534 -> SDP 0x11B564). For the DualSense's
connection record - state +0xC = 1, flags +4 = 1 (bit 0x40 clear) in all three dumps - the
generic route goes to SDP, which is what the limbo dump's populated SDP buffers show.

**What the manual experiment changed**: the second inline compare (0x11B5DC) and the helper's
`xori` (0x29E2CC) went from 0x042F to 0x0CE6. The helper then returns 1 for (0x054C, 0x0CE6), so
the factory builds the object (0x650800, class 0x38CDC8, 054C/0CE6 at +0x68); the inline block
routes the DualSense straight to 0x11B234. The price was Move navigation (0x042F), unrecognised
until reboot. The dumps cannot separate the two edits (both were made together), so the shim
reproduces both.

**Dump comparison (why one formed and the other did not):**

| observation | shim dump | limbo dump | manual dump |
|---|---|---|---|
| heap qword == 0x800000000038CDC8 | none | 0x661000 (a DS3) | 0x650800 (DualSense) and 0x661000 |
| SDP response buffers 0x434E10..0x435250 | all zero | populated | populated |
| shared RX buffer 0x4326C8 | all zero | residue | `02` handshake byte |
| connection entry 0 (0x4B0170) +0x08 / +0x2C | 0 / 3 | 2 / 1 | 2 / 1 |
| link-key entry flags (word at 0x4349D8) | 0x0000D201 | 0x0000D601 | 0x0000D601 |
| qwords equal to 0x800000000011B528 | 5 | 0 | 0 |

The shim dump is the only one where neither route ran; the semantics of the connection-record
and flag deltas are not decoded and are listed only as observed differences.

## 4. The fix

### 4.1 Recommended - Option A: WORD rows at the inline site, helper BRANCH unchanged

`pid_patches` grows from 2 rows to 3 (new order shown; the new FNV for the second row is the
hash of qword 0x11B5E8 in the pristine kernel):

```c
	/* F4 - product-ID acceptance (see 2026-10-31-ds3-route-pid-patch-fix.md).
	 *   [0] inline site, qword 0x11B5E0: the third compare (0x03D5) becomes
	 *       cmpwi cr7,r0,0x0CE6; the bne cr7,0x11B534 after it is kept.
	 *   [1] inline site, qword 0x11B5E8: the fall-through "b 0x11B564" becomes
	 *       "b 0x11B234" (known Sony pad, skip SDP) - reached only for PID 0x0CE6.
	 *   [2] helper 0x29E2B4 first instruction -> BRANCH to shim_pid_ok (unchanged; value
	 *       filled with blob base + OFF_PID_OK at install). Function entry of a leaf
	 *       with shim_pid_ok's ABI: the only kind of site a BRANCH row may target. */
	{
		{ DS45_KPATCH_WORD,   0x800000000011B5E4ULL, 0xE8ADD0EDF3B2B3C1ULL, 0x2F800CE6ULL },
		{ DS45_KPATCH_WORD,   0x800000000011B5ECULL, 0xF86140C9C3D7FB8DULL, 0x4BFFFC48ULL },
		{ DS45_KPATCH_BRANCH, 0x800000000029E2B4ULL, 0xF1B8B3EFF22A78FCULL, 0ULL },
	},
```

Checks run on the pristine image in memory: each `expect` equals the FNV of its original
qword (0x11B5E0, 0x11B5E8, 0x29E2B0); the patched block decodes to `cmpwi cr7,r0,0xCE6` at
0x11B5E4 and `b 0x11B234` at 0x11B5EC with the `beq`s and the `bne cr7,0x11B534` untouched.
Write order is safe at every intermediate step (after row 0 alone, PID 0x0CE6 still reaches the
old `b 0x11B564`; after row 1 alone, PID 0x03D5 would be accepted for a moment). Reverting
restores the originals (`kpatch_revert`, last first).

Post-patch qword hashes for the passive re-test: qword 0x11B5E0 = 0xE89B9FEDF3A23A4B, qword
0x11B5E8 = 0xF86466C9C3DA71F6.

### 4.2 Control flow after the patch

Unchanged: PID 0x0268 and 0x042F -> `beq` -> 0x11B234. New: PID 0x0CE6 -> compare equal,
`bne` not taken -> `b 0x11B234`, byte-for-byte the destination and register state of the
`beq` the proven experiment used. Everything else -> `bne` -> 0x11B534 (r9 = VID intact).
The helper accepts 0x0268, 0x042F and 0x0CE6 through `shim_pid_ok`, so the factory builds the
object as in the manual dump.

### 4.3 Why this, and the one behavioural delta

- **Simplest robust.** No code runs in the blob at the inline site, so nothing can touch LR,
  clobber a register or mis-route a branch; the site goes back to the kernel's own control
  flow. The only blob-executing patch left is a true leaf at a function entry.
- **Delta.** The kernel special-cases PID 0x03D5 (Move motion controller) straight to the SDP
  request; with row 0/1 that PID takes the generic decision like any unknown Sony PID.
  For a connection in the normal state (+0xC = 1, flag 0x40 clear - the DualSense's, and the
  state in which the generic route is already known to end in SDP) the outcome is the same
  SDP request; it differs only if the connection state is not 1 or a name match set 0x40, in
  which case the pad would be treated as a known Sony pad instead. No Move motion controller
  has been on this console; if one is ever used, re-test it. Removing the delta needs Option B.
- **Alternatives rejected.** *A0*, the single proven word (WORD 0x800000000011B5DC, expect
  0x82EB2E1F6C9B0080, value 0x2F800CE6): fewest changes, but it makes 0x042F fall through to
  the generic decision at the inline site (a Move navigation pad is then accepted only if the
  name flag happens to be set), which violates "keep Move navigation". *WORD-only helper*:
  keeping 0x042F and adding 0x0CE6 inside the 16-instruction helper needs about five word
  rows (a `mullw` of two xor results) - more patch surface than the proven leaf thunk.
  *Helper-only (no inline row)*: untested; the generic route ends in SDP, which the limbo dump
  shows completing, but only the combined configuration is proven.

### 4.4 Option B (not recommended): a thunk at 0x11B5E4

Only worth it to keep 0x03D5 exact. `shim_pid_ok` can **never** be used here, and the thunk
needs its own blob entry (`shim_pid_inline`), distinct from `shim_pid_ok`:

- **In** (reached by `b`, so LR is stale and must not be used): r0 = PID (0x0268/0x042F never
  arrive), r9 = VID, r28 = connection object, r1 = live frame, r2 = kernel TOC; CR, CTR, LR dead.
- **Out** (tail-jump, never return): PID == 0x0CE6 -> 0x11B234 (needs r28); PID == 0x03D5 ->
  0x11B564; otherwise -> 0x11B534 (needs r28 and **r9 = VID intact**).
- **Preserve** r1, r2, r9, r28 (read by all three targets) and every callee-saved register the
  handler's prologue did not save (r14-r24, r30, r31); may clobber r0, r3-r8, r10-r12, CR,
  CTR, LR.
- It cannot be written as freestanding C: it never returns and jumps to three absolute kernel
  addresses. It needs a small hand-written entry, a new exported offset in `shim_pack.py` and
  `shim_blob.h`, a second BRANCH row with its own target in `kshim.c`, and new tests.

### 4.5 Edits the implementer must make around the rows

1. `kshim.h`: `pid_patches[2]` -> `[3]`. `kshim.c`: verify all rows with `hash_ok` (loop, not two
   calls); build `final[]` from them but set `.value = target` and run the 26-bit range check
   **only for BRANCH rows**; `final[]` holds 5 (3 + 2 redirects, `DS45_KPATCH_MAX` is 8); the
   journal then has 5 entries (update the "all four" comment). `test_kshim.c`: new row shape.
2. `shim.c`: delete the sentence that 0x03D5 is accepted at the inline site - no longer true.
   `shim.h`/`kshim_table.c` comments: the inline site is now WORD rows.
3. Optional hardening: bump `DS45_SHIM_VERSION` so a corrected build never adopts a previous
   build's install (adoption keeps the old wedging branch live); and flush the instruction
   cache over the patched kernel text after `kpatch_apply` (today only the blob is flushed,
   before the patches are written).
4. Process: a BRANCH row is only valid at a function's first instruction, or with a thunk that
   proves the site's full register and control contract. State that in `kshim_table.c`'s
   header; host tests cannot catch it because they call `shim_pid_ok` in isolation.

## 5. Re-test (passive, per the native-DS3 standard)

Reboot first - the wedge and the old patches are RAM state. Same DEX-session setup (link-key
type 0x01 and the 8 armed slots; the type flip is still a manual step, not part of this fix).
Connect the DualSense, take the usual dump and trace. **Pass for this fix:**

1. A heap qword 0x800000000038CDC8 exists besides the static vtable at 0x378388, with
   054C/0CE6 at +0x68 of that object - the pad object for the DualSense.
2. No qword equal to 0x800000000011B528 outside the code image (no wedge frames).
3. Hashes of qwords 0x11B5E0 and 0x11B5E8 equal the post-patch values in 4.1 (patch live).
4. Informational: the SDP buffers at 0x434E10..0x435250 stay zero (the Sony route skips SDP).
5. Regression: a DS3 still gets its pad object; a DS4 connecting (VID 0x054C) leaves no loop
   frame and BT stays responsive.

The next milestone after this (R2: `stat_startups` > 0, the 0x31 stream, `stat_translated`)
is separate - the manual dump's pad object stalled in start-up, which is what the F1/F2 hooks
are for.

## 6. Residual risks

- Static only: Option A is verified by decoding and by simulating the decision block, not on
  the console.
- The 0x03D5 delta above; the meaning of the connection-record and link-key-flag deltas in
  section 3 is undecoded.
- The helper thunk has not executed on the console yet; its ABI fit is verified for all six
  call sites, but the first factory call is also its first live run.
- Corrections to `2026-10-30-ds3-route-facts.md` F4: the "BRANCH redirect at both sites"
  decision is superseded for the inline site (its register-contract paragraph was right, except
  that r9 is live into 0x11B534); and the helper's callers there ("0x2A08A4, 0x2A1854,
  0x2A19D4 (pad-creation paths)") are incomplete - the actual creation gate is the factory's
  call at 0x2A14B0, which the call graph missed, as it did 0x2A20C8 and 0x2A23A0.
