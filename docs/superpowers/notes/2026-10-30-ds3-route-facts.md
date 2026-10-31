# DS3-route shim facts — DEX 4.92 (read-only research, plan task 1)

Scope: facts a later task needs to write the in-kernel shim that runs a DualSense
through the PS3's own Sony-pad (DS3) Bluetooth driver on CFW DEX 4.92.
All values settled read-only from `analysis/ff_492.json`,
`analysis/lv2_code_492.bin` (code, LV2 offset = byte offset) and the full 8 MiB
dump `tools/dumps/dex492/ds45pad_lv2.bin` (LV2 offset = byte offset).

Conventions:
- Kernel addresses written as `0x80000000xxxxxxxx`; a bare `0xNNNN` is an LV2 offset.
- Code that the installer must verify before patching is identified by the
  **FNV-1a-64** hash of the 8-aligned qword that contains it
  (`h=0xCBF29CE484222325`; per byte `h^=b; h=(h*0x100000001B3) mod 2^64`), plus a
  prose/opcode-mask description. No raw kernel bytes are reproduced here.
- Pointer slots are data; their expected value is given as a pointer.
- The TOC (r2) for all kernel OPDs on this firmware is `0x80000000003759C0`.

---

## F1 — Start-up hook (Bluetooth Sony-pad start-up)

**Hook slot (pointer, atomic 8-byte write):** `0x800000000037CFE8`
— LV2 `0x37CFE8`, slot 7 of the Bluetooth per-transport descriptor table at `0x37CFB0`.
**Current value:** `0x800000000033A208` (an OPD; its entry qword is
`0x80000000000CA194`, TOC `0x80000000003759C0`).

**Why this slot is the Bluetooth start-up (evidence):**
There are three sibling 9-slot per-transport descriptor tables, each with a slot-7
start-up dispatcher:
- `0x37CF20` slot 7 (`@0x37CF58`) → OPD `0x33A280` → fn `0x800000000000CAA3C`
- `0x37CF68` slot 7 (`@0x37CFA0`) → OPD `0x33A298` → fn `0x800000000000CAB84`
- `0x37CFB0` slot 7 (`@0x37CFE8`) → OPD `0x33A208` → fn `0x800000000000CA194`

`0x0CAA3C` and `0x0CAB84` send only through `0x80000000000107C18` (the non-Bluetooth
sender; needs global ready flag `*0x432BF8`). `0x0CA194` is a substate dispatcher on
`drv+0x54` (jump table at OPD-derived base `0x800000000000CA218`, substates 0..8);
its state-7 handler `0x0CA2CC` is the only one of the three families that reaches the
**Bluetooth** SET_REPORT sender `0x80000000000102750` (direct `bl` at `0x0CA334`,
confirmed by a bl-target scan: `0x102750` is called only from `0x0CA334` and
`0x0CA9BC`, both inside the `0x0CA194` substate machine). So table `0x37CFB0` is the
Bluetooth transport's table and `@0x37CFE8` is its start-up slot.

**Prototype of the pointed-to function** `0x80000000000CA194`:
```
int ca194(void *drv /*r3*/, long arg1 /*r4*/);   // returns int (0, or negative LV2 error e.g. 0x8001000C)
```
It early-returns when `arg1==0`; otherwise dispatches on `drv+0x54` (byte, bounded ≤8).
Relevant driver-object fields observed: `+0x38` state (dispatched by `0x0CAB84`),
`+0x54/+0x55` substate, `+0x58` → sub-object, `+0x60` feature-report build buffer,
`+0x98` enable flag, `+0xAC` **Bluetooth connection handle byte** (the value the
senders pass to the connection lookup). This driver/handshake object is distinct from
both the 16-byte transport object (pad+0x4F0) and the pad object (class `0x38CDC8`).

**Getting VID/PID from the arguments:** NOT directly present in `drv`. The only
identity field in `drv` is the connection handle at `+0xAC`. VID/PID is reachable by
resolving that handle to its connection record (the same lookup `0x801010B8` that the
senders use internally), or from the pad object (`pad+0x68` = VID/PID, proven in the
dumps: live DS3 pad `0x654800`+0x68 = `054C/0268`; the limbo DualSense pad `0x650800`
+0x68 = `054C/0CE6`). The exact `drv`→pad / `drv`→conn linkage and the VID/PID field
offset inside the connection record are NOT established statically — see Unsatisfiable
U1.

**Condition that advances the pad object to running (settled):**
It is **a reply from the pad (a real input report), NOT the start-up send's return
value.** The pad handler `0x800000000002A4E40` (pad obj in r28, transport obj at
pad+0x4F0, class `0x37CE18`, method 4 = input read) advances only when an input report
arrives with **byte[0]==0x01 and byte[1]!=0xFF** (checks at `0x2A4F7C`/`0x2A4F84` and
`0x2A4F88`/`0x2A4F90`). On that path it calls `0x2A26A8` → `0x2A2710` → `0x0C9AF0`
(which records the report length into the object), sets the "first report seen"
bookkeeping (`+0xF4` cleared, `+0x142`=1), copies the report into the object at `+0x22`
and runs the converter. No valid `0x01` report ever reaches the shared buffer for a
DualSense (console capture: only a single `0x02` handshake byte at connect, then
silence), so it stalls. `drv+0x55` is set to 1 by the start-up state machine
(`0x0CA358`) and `pad+0x21` is a **slot number**, not a handshake state (per the known
facts correction).

**Decision — what the wrapper must do for a DualSense to reach running:**
On the Bluetooth start-up (this slot), for a connection whose VID/PID is `054C/0CE6`,
do the DualSense's own enable instead of the DS3 `0xF4` SET_REPORT — issue a Bluetooth
HID GET_REPORT of feature `0x05` (see F3), which switches the pad to full report
`0x31` — then let the driver's state machine continue as after a DS3 acknowledgement
(tail-call/return success so the pad object is kept and polled). The actual
3→running transition is then produced by the input wrapper (F2) translating each
`0x31` report into a DS3 `0x01` report, which satisfies the handler's
`byte[0]==0x01 && byte[1]!=0xFF` gate and assigns a port. For any non-DualSense the
wrapper tail-calls the original `0x0CA194` so a DS3 is untouched.

Verification hash (installer sanity-check of the pointed-to function identity):
- `0x0CA194` entry qword `@0xCA190`: FNV `0x3EDBE61A32005B76` (start-up dispatcher
  prologue: stdu/mflr, `cmpdi r4,0`).

---

## F2 — Input hook (transport method 4)

**Hook slot (pointer, atomic 8-byte write):** `0x800000000037CE38`
— method 4 of the Bluetooth transport vtable `0x37CE18` (vtable+0x20).
**Current value:** `0x800000000033A010` (OPD; entry `0x80000000000C9D60`, TOC as above).
Evidence: the transport object (pad+0x4F0) is built by `0x0C9EF4` and its vtable is
`0x37CE18` (the only reference to that class is TOC slot `0x3716D8`, loaded at
`0x0C9EF4`); handler `0x2A4E40` dispatches input via `*(pad+0x4F0)` → `vtable+0x20` →
OPD `0x33A010` → `0x0C9D60`.

**Prototype of `0x80000000000C9D60`:**
```
int c9d60(void *xport /*r3*/, int r4, unsigned short *len_out /*r5*/, int flag /*r6*/);
// returns byte count read (sign-extended); the report lands in the driver's shared buffer
```
It reads the transport handle at `xport+8`, flips `flag` (`xori r6,r6,1`), and calls
`0x80000000000101E20(handle, r4, &len, flag)`. `0x101E20` locks the BT-HID lock
(`*0x4325D0`), looks the connection up by handle (`0x801010B8`) and calls `0x0F8FE0`.

**Does the result buffer start with the report ID or 0xA1?** — **the report ID.**
`0x0F8FE0` memsets the shared buffer `0x4326C8` to `0x41` bytes, calls
`0x80000000000116EF0(bdaddr,1,2,0x4326C8,0x41)` to drain the newest queued input, then
copies **from `0x4326C9` (i.e. it strips the leading `0xA1` DATA header)** `len-1`
bytes to the caller. So the caller sees `report[0] = 0x01` for a DS3 (and would see
`0x31` for a DualSense), never `0xA1`.

**How many bytes a DualSense report keeps (queue limit vs the 0x41 buffer):**
The shared buffer and the queue read are both capped at **`0x41` = 65 bytes**
(`0x0F8FE0` memsets 0x41 and passes 0x41 to the reader). After the `0xA1` header is
stripped, the handler path sees at most **64 bytes** of report. A DualSense Bluetooth
`0x31` report is 78 bytes, so it is truncated to 64; everything the translator needs
(report id, buttons, hat, sticks, L2/R2 analog, gyro/accel) is within the first 64
bytes and the report CRC (last 4 bytes) is dropped. This matches the spec's "the
0x41-byte buffer holds everything used but not the CRC".

**Getting VID/PID from its arguments:** only the transport handle (`xport+8`) is in
reach here; VID/PID again requires the handle→connection resolution (or the pad
object's `+0x68`). Same residual as F1/U1. Practical note for the implementer: the
decision "is this a DualSense" is most robustly taken from the pad object `+0x68`
(VID/PID, proven) or a per-connection flag the plugin sets at install, rather than
from the 16-byte transport object.

Verification hash:
- `0x0C9D60` entry qword `@0xC9D60`: FNV `0x5D4E4578D571F2AE` (input-read thunk
  prologue: stdu/mflr, `sth r5,0x70(r1)`).

---

## F3 — GET_REPORT of feature 0x05

**Finding: there is NO ready-made Bluetooth HID GET_REPORT helper in this driver, and
`0x102750` cannot be used for GET.** `0x102750`→`0x0F8BD8` hard-codes the transaction
header byte as `(reportType | 0x50)` (SET_REPORT; `0x53` for feature), writing it to
buffer byte 0 and the report id to byte 1. A full scan of the HID region found no
`ori ...,0x43` (GET_REPORT|feature) anywhere and only one `ori ...,0x40` (at `0x106C24`,
which is an unrelated attribute flag bit, not a HID header). The DS3 driver only ever
issues SET_REPORT (feature `0xF4` enable, output `0x01` LED/rumble), consistent with
how a real DS3 is initialised.

**Reference for why feature 0x05 is the right enable:** Linux `hid-playstation.c` —
reading HID **feature report 0x05** (the calibration report) over Bluetooth is what
switches a DualSense from its minimal BT input report to the full motion report
`0x31`. (Cited as public reference; the kernel driver here does not implement it.)

**Mechanism the shim must use (partially settled; one call does not exist):**
Build the Bluetooth HID control-channel GET_REPORT PDU by hand — header byte
`0x43` (`GET_REPORT`=0x40 | `feature`=0x03), report id `0x05` — and transmit it on the
same HID control path the SET sender uses. The lowest reusable primitive inside
`0x0F8BD8` is the HCI/ACL assembly+send around `0x80000000000082A08` (payload memcpy
after the 2-byte header) feeding `0x80000000000116EF0`; a GET differs from `0x0F8BD8`
only in the header byte (`0x43` instead of `(type|0x50)`) and in expecting a returned
feature report. Because `0x0F8BD8` hard-codes `0x50`, the shim must replicate its
send sequence with the `0x43` header rather than call it. Whether it may be called
from the F1 (start-up) context: yes — the F1 slot runs on a driver thread that already
holds / can take the BT-HID lock (`*0x4325D0`), the same context the SET senders run
in; a single non-blocking control-channel write with no wait is within the shim rules.

**What is not settled read-only:** the exact prototype/args of the control-channel
send primitive to reuse for a GET (versus SET), and empirical confirmation that a
feature-0x05 GET actually flips this firmware's DualSense to `0x31`. See Unsatisfiable
U2.

Verification hashes (SET sender, for reference / to confirm the driver build):
- `0x102750` is an OPD-called fn (OPD `0x3408C8`); callers of the BT sender are
  `0x0CA334` and `0x0CA9BC` only.

---

## F4 — Product-ID acceptance

Goal: accept `0x0CE6` **in addition to** the IDs the kernel already accepts, keeping
DS3 `0x0268` and Move-navigation `0x042F`. **Keeping `0x042F` is possible at both
sites** (the gate "cannot keep 0x042F" is NOT triggered). Chosen kind at both:
**BRANCH redirect to a shim blob** (the spec's preferred mechanism). The blob returns
"accept" for `054C` with PID ∈ {`0x0268`, `0x042F`, `0x0CE6`} (and `0x03D5` at the
inline site), so no existing ID is lost. The blob must sit within a 26-bit relative
branch of each site (satisfied — see F6).

**Inline site, func `0x11B098` (BT connection/SDP setup):**
After `VID==0x054C` is confirmed (`0x11B52C`), the PID (half-word at `sp+0xB6`) is
tested by three consecutive `cmpwi`/branch pairs:
`0x11B5D4 cmpwi 0x268` → `0x11B5D8 beq 0x11B234` (known Sony: skip SDP);
`0x11B5DC cmpwi 0x42F` → `0x11B5E0 beq 0x11B234`;
`0x11B5E4 cmpwi 0x3D5` → `0x11B5E8 bne 0x11B534` (else fall through to `0x11B564` SDP).
Patch: replace the `0x03D5` compare instruction at **`0x800000000011B5E4`** with a
branch to the shim PID blob, which re-tests `0x03D5` and `0x0CE6` (and preserves the
`0x0268`/`0x042F` behaviour already matched above it), branching to `0x11B234` for a
DualSense and otherwise reproducing the original control flow
(`0x3D5`→`0x11B564`, else→`0x11B534`). The two earlier compares (`0x0268`, `0x042F`)
are left intact.
Verification hashes (installer must match before patching):
- qword `@0x11B5D0` (holds `0x11B5D4 cmpwi 0x268` + `0x11B5D8 beq`): FNV
  `0x1D9D36267FF6017D`.
- qword `@0x11B5D8` (holds `0x11B5D8 beq` + `0x11B5DC cmpwi 0x42F`): FNV
  `0x82EB2E1F6C9B0080`.
- qword `@0x11B5E0` (holds `0x11B5E0 beq` + `0x11B5E4 cmpwi 0x3D5`, the patch target):
  FNV `0xE8ADD0EDF3B2B3C1`.

**Helper `0x29E2B4`** `int(r3=VID, r4=PID)` → returns 1 for `VID==0x054C &&
(PID==0x0268 || PID==0x042F)`, else 0 (the `0x042F` test is the `xori r4,r4,0x42F` at
`0x29E2CC`). Callers: `0x2A08A4`, `0x2A1854`, `0x2A19D4` (pad-creation paths).
Patch: replace the first instruction at **`0x800000000029E2B4`** with a branch to the
shim PID blob, which returns 1 for `054C` with PID ∈ {`0x0268`,`0x042F`,`0x0CE6`} and 0
otherwise, then returns to the caller (leaf function, so the blob just sets r3 and
`blr`). Keeps `0x0268` and `0x042F`.
Verification hashes:
- qword `@0x29E2B0` (entry: `cmpwi r3,0x54C` + `cmpwi r4,0x268`): FNV
  `0xF1B8B3EFF22A78FC`.
- qword `@0x29E2C8` (`blr` + `xori r0,r4,0x42F` tail): FNV `0x8D1047F832C76096`.

---

## F5 — Instruction-cache coherency for freshly written code

The CFW syscall-7 poke is RAM-only and does **not** guarantee I-cache coherency for
code (the on-console experiments noted stale-I-cache as a likely reason a code poke did
not take effect). Use the kernel's own range-flush primitive.

**Routine:** LV2 `0x29B8`, kernel address **`0x80000000000029B8`**.
```
void icache_flush(void *addr /*r3*/, unsigned long len /*r4*/);
```
It loops over `[addr, addr+len)` in 128-byte (`0x80`) steps doing
`dcbst; sync; icbi; isync` per line — the full data-store / I-cache-invalidate
sequence, i.e. exactly the coherency barrier needed after writing code. It is a leaf
with existing kernel callers (`0x1950`, `0x2294`, `0x23AC`).
**It has no OPD** (called directly in-kernel). To invoke it through the spare syscall
slot (F6), the installer points the slot at an OPD it places in its own data block,
`{ entry=0x80000000000029B8, toc=0x80000000003759C0 }`, calls the syscall with
`(r3=addr, r4=len)`, then restores the slot.
Verification hash:
- `0x29B8` entry qword `@0x29B8`: FNV `0x56636AA045D470E1`
  (`add r4,r3,r4` + `rldicr r3,r3,0,0x38`).

---

## F6 — Allocation (allocator, syscall table, spare slot, pool)

**Syscall table:** base **`0x800000000038A4E8`**, 1024 entries × 8 bytes; each entry is
a **pointer to an OPD** (dispatcher at `0x27DD70`: it indexes `table[num]` for
`num<0x400`, then double-derefs `table[num]`→OPD→entry and calls via `mtlr/blrl` with a
pre-loaded kernel r2). The table base is computed at runtime from SPRG0, so the static
address above is the array located in the dump by its content; the installer must still
verify a known slot before use. Confirmed correct because slots 6–11 point into the CFW
payload region (`0x5600xx`), i.e. the CFW-installed peek/poke syscalls (known facts:
CFW uses 6/7/8/9/10/11, 35).

**Not-implemented stub:** OPD **`0x80000000003621B0`** (entry `0x800000000002742D4`);
**320** table slots point at it.

**A spare (unused) syscall and its current value:**
- `sys 149` — slot address **`0x800000000038A990`**, current value
  `0x80000000003621B0` (the ni stub). Other clearly-unused slots: 162, 164, 168, 183,
  189, 195, 200–208, … (all currently `0x3621B0`). The installer must read the chosen
  slot and confirm it equals `0x80000000003621B0` before using it, and restore it
  after.
Verification hash for the stub target:
- ni-stub entry qword `@0x2742D0`: FNV `0xC41A1A7A8718064D`.

**LV2 allocator used by the driver itself:**
The pad constructor `0x2A3F08` obtains its sub-object via `0x681F4`
(`0x2A4058`: `r3=0x10, r4=0x22, r5=0`), and the pad handler allocates a pad object the
same way (`0x2A5094`: `r3=0x1C0, r4=0x22, r5=0`).
**Descriptor (OPD): `0x8000000000330998`** (entry `0x80000000000681F4`, TOC as above;
duplicate OPDs also at `0x2A68`, `0x4108`, `0x575A70`).
```
void *alloc(unsigned long size /*r3*/, unsigned long pool /*r4*/, unsigned long flags /*r5*/);
// returns NULL on failure (constructor checks r3==0)
```
**Pool value to use: `0x22`** — the same pool the driver uses for its own objects.
Objects from this pool land in the kernel heap around `0x6xxxxx` (observed live pad
objects: `0x650800`, `0x654800`, `0x661000`, `0x690400`, `0x66DC00`). That region is
within a **26-bit relative branch** (±0x2000000 = ±32 MiB) of both F4 patch sites
(`0x11B5E4`→`0x6xxxxx` ≈ 5.5 MiB; `0x29E2B4`→`0x6xxxxx` ≈ 3.9 MiB), so a blob allocated
from pool `0x22` is reachable by the F4 inline branches. Caveat: pool `0x22` is the
driver's data pool; see U3 on executability.
Verification hash:
- `0x681F4` entry qword `@0x681F0`: FNV `0x6ADFF6033ABA740D`.

---

## Unsatisfiable list (needs more than read-only static analysis)

**U1 — VID/PID from the F1/F2 hook arguments (exact field).**
Both hook points receive an object that carries only a Bluetooth handle (start-up
`drv+0xAC`; transport object `xport+8`), not VID/PID. The handle→connection-record
mapping and the VID/PID field offset inside the connection record were not pinned down
statically (the dispatch into the driver object is all indirect/method-table, which the
earlier work already found "bottomed out"). VID/PID is reliably available at
`pad_obj+0x68` (proven in the dumps), so the implementer can key the decision off the
pad object or a plugin-set per-connection flag. *What would settle the exact arg path:*
a read-only console snapshot that walks `drv`/transport→connection→VID/PID for a live
DualSense (e.g. extend the existing `navpid_connected.bin`-style capture), or a
one-time instrumented read — no write needed.

**U2 — F3 GET_REPORT feature 0x05 (no one-call helper; empirical switch unconfirmed).**
The driver exposes no Bluetooth HID GET_REPORT path; `0x102750`/`0x0F8BD8` are SET-only
(header hard-coded `|0x50`). The shim must assemble the GET PDU (`0x43`, id `0x05`) and
drive the control-channel send primitive itself. The exact send-primitive prototype for
a GET (vs SET) and confirmation that feature-0x05 GET flips *this* DualSense to report
`0x31` cannot be settled from the static dump. *What would settle it:* a console
write-experiment (send a feature-0x05 GET on the HID control channel to a connected
DualSense and observe whether `0x31` reports then appear) — **not performed** (write
action, out of scope for this task).

**U3 — Executability of pool-0x22 memory.**
Reachability by 26-bit branch from the F4 sites is settled (above). Whether pool `0x22`
returns memory the CPU will execute (the LV2 kernel heap is executable on CFW in
practice, and the design already flushes the I-cache via F5 after writing) is not
provable from the dump. *What would settle it:* a read-only console check that code
placed in a pool-0x22 allocation executes after an F5 flush, or selection of a pool
known-executable — needs a console run.

---

## Self-review

- Every F1–F6 field above carries its evidence (function address, instruction offset,
  observed pointer value, and a reproducible FNV hash for each code site).
- F2 and the input/running mechanics are fully grounded in disassembly and the dumps;
  an implementer can write the input-wrapper prototype from F2 alone.
- F1's wrapper prototype (`int ca194(void *drv, long arg1)`) and hook slot are settled;
  the one gap for an implementer is deriving VID/PID (U1) — the note gives the proven
  fallback (`pad+0x68`).
- F4 keeps `0x042F` at both sites (gate not triggered); F5 and F6 are fully settled.
- Three residuals (U1–U3) are listed with the exact experiment that would settle each;
  per the task gate, a non-empty Unsatisfiable list means the controller should stop and
  involve the user before Task 6.
