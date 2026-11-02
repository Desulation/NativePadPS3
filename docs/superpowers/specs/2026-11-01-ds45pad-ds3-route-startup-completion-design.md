# DS45Pad native DS3 route: start-up completion and basic input (stage 1)

Date: 2026-11-01. Status: approved; amended the same day after plan task 1
(see "Amendment after plan task 1").

Amends `2026-10-30-ds45pad-native-ds3-route-design.md` (spec 1). This spec
**replaces** spec 1's "Start-up" section and the parts of "Components" and
"Install" that served it. Spec 1's input translation of report `0x31`, USB
pairing, shim code rules and constraints still apply unless changed here.

## Goal

A DualSense registered as a Sony pad finishes the DS3 driver's Bluetooth
start-up exactly as a DS3 does, so the PS3 keeps its pad object and assigns it
a port, and the pad's buttons, sticks and analog triggers work through the
native DS3 path. This is stage 1 of the start-up work: basic input only.

## Constraints (from the user)

- Works like a DS3 or better; never asks the user anything; verification is
  passive only.
- All kernel writes are RAM-only, on fingerprinted kernels only (DEX 4.92
  first), verified against the expected original before writing and read back
  after. The shim stays behind the `ENABLE_SHIM` gate (default 0).
- Nothing Sony-proprietary in git: no SDK files, keys, dumps, kernel code bytes
  or SDK-linked binaries. Expected original kernel code is stored as hashes;
  kernel addresses and small device constants (version numbers, flags,
  calibration values) may be stored as values.
- A DS3, and any other device, is untouched.
- Staged: basic input first, with no requests sent by our kernel code. Stage 2
  adds full reports (motion, touchpad) and USB-cable pairing.

## What is known (DEX 4.92)

Mapped read-only and confirmed on the console on 2026-11-01 (details in the SDD
ledger, sections "READ-ONLY (b) DONE", "CONSOLE CAPTURE 2026-11-01" and
"START-UP DRIVER MAPPED"):

- With the PID acceptance patch and link-key type `0x01`, the PS3 **does**
  create a Sony pad object for a DualSense (vtable `0x38CDC8`, `054C/0CE6` at
  +0x68). It is removed about 0.89 s later, reproducibly (three runs).
- Each pad gets a Bluetooth start-up driver object, built by `0xCB1D0`:
  vtable `0x37CFC8`, +0x38 state, +0x3C retry budget (32), +0x40 ready
  callback, +0x48 pad object, +0x50 status, +0x54 substate, +0x58 pointer to
  the pad's info block (pad+0x80), +0xAC connection handle.
- Driver methods: +0x20 `0xCA194` sends the request for the current substate
  (slot `0x37CFE8`); +0x28 `0xC9F28` "done" = substate 9; +0x30 `0xCACF0` is
  the event handler (slot `0x37CFF8`).
- Framework `0xCB5C0`, per event: calls +0x30 with the transfer status. A
  non-zero return is retried every 20 ms until the budget runs out. A zero
  return with substate 9 completes. Either way it then calls the ready callback
  with status = drv+0x50.
- The event handler stores the transfer status in drv+0x50 first. A refused
  request (non-zero status) is retried. On success it parses the DS3's reply
  into the info block (register version at +0x00, firmware version at +0x02,
  address bytes at +0x08..+0x0F, further fields up to +0x55).
- Ready callback with non-zero status: the pad manager removes the pad.
  Status 0: the pad's check method `0x2A3AD8` requires register version > 2
  and != 0x817 and firmware version > 0x32F, then assigns the port. A real DS3
  has 0x0005 / 0x0350.
- So a DualSense refuses every DS3 start-up request and is removed after 32
  retries. Its info block stays zero. (That the refusal arrives as a non-zero
  transfer status is inferred from timing and the earlier `0x02` handshake
  capture; it was not observed directly.)
- The second console hang was caused by our kernel code on the connect path
  (the start-up hook and/or the helper branch), not by the inline PID edits.

## Amendment after plan task 1 (2026-11-01)

Plan task 1 (`docs/superpowers/notes/2026-11-02-ds3-route-startup-completion-facts.md`)
settled the facts below; the user chose the three changes on 2026-11-01. The
sections that follow are updated to match.

- **Identity bytes are per pad (C1).** Seven bytes of the info block
  (pad+0x88..+0x8C, +0x8E, +0x8F) are the controller's identity. The pad manager
  closes a new Bluetooth pad whose identity equals a pad that already has a port.
  Decision: `shim_event` fills them from a fixed prefix plus a per-boot
  completion counter kept in the data block. No plugin involvement.
- **The transport is not embedded (C2).** pad+0x4F0 holds a pointer to a
  separate 16-byte heap object with no pointer back to its pad, and its handle
  byte is 0 for every Bluetooth pad. Decision: the input hook finds the pad by
  scanning the pad manager's 16 object pointers for the one whose +0x4F0 equals
  the transport.
- **A second driver gates the port (C3).** After the check passes, the kernel
  starts a second driver (vtable `0x37CF80`) that sends feature report `0xF4`;
  the port is assigned only if it completes with status 0, and a refusal leaves
  the pad connected with no port. Decision: a second completion hook,
  `shim_enable`, on that driver's event slot `0x37CFB0`. A refused output report
  (LEDs, rumble) cannot remove the pad, so no output wrapper is needed.

## Design

### 1. Start-up completion (kernel side)

**Hooks.** Two redirects. The start-up driver's event-handler slot `0x37CFF8`
(original OPD `0x33A2B0`, function `0xCACF0`) points at `shim_event`. The
enable driver's event-handler slot `0x37CFB0` (original OPD `0x33A310`,
function `0xCB234`) points at `shim_enable`. The current start-up hook on
`0x37CFE8` is removed, together with its feature-0x05 send, the per-connection
id slots and the retry counter.

**Identity.** `shim_event(drv, status, len)` reads `pad = *(drv+0x48)`. If
`pad` is null, its vtable is not `0x38CDC8`, `pad+0x68` is not `054C/0CE6`, or
the shim is disabled, it tail-calls the original handler.

**Completion,** for a DualSense whose start-up is not yet complete (substate
!= 9), on its first event whatever the status:
1. Copy the DS3 info template from the data block to `*(drv+0x58)` (pad+0x80).
2. Increment the completion counter and write the pad's identity bytes (below).
3. Set drv+0x50 = 0 (status) and drv+0x54 = 9 (done).
4. Return 0.

The framework then sees "done" and calls the ready callback with status 0; the
kernel's own version check passes and the pad manager starts the enable driver.
A later event for the same driver (substate already 9) goes to the original.

**Enable completion.** `shim_enable(drv, status, len)` uses the same identity
test on `*(drv+0x48)`. For a DualSense whose enable driver is not yet done
(byte drv+0x96 == 0), on its first event whatever the status: set drv+0x50 = 0
and drv+0x96 = 1, return 0. The framework calls the callback with status 0 and
the pad manager assigns the port. Anything else tail-calls the original. The
kernel still sends the `0xF4` request itself; the shim neither adds nor
suppresses sends. If a DualSense never answers it at all, the enable driver
never completes; console test step 2 watches for that.

**Rules.** No sends, no locks, no allocation, no calls into the kernel apart
from the tail-call to the original. Both hooks only store into the driver
object and its info block, which the original handlers already write on this
same thread. Straight-line, bounded code.

**Template.** The DS3 info block from plan task 1 (note section C1.5), stored as
plain constants in the data block (version numbers, flags, calibration values),
with the identity bytes zero.

**Identity bytes.** Written per pad after the template copy, big-endian:
pad+0x88..+0x8B = the completion counter after the increment (first DualSense
of a boot = 1), pad+0x8C = `0xD5`, pad+0x8D = 0, pad+0x8E..+0x8F = `0x44 0x53`
("DS"). Unique within a boot, never the `0x000D` value that takes the check's
alternate branch. A reconnecting DualSense gets a new identity, so the kernel's
"same controller reconnected" match never applies to it; the old pad is
removed on disconnect, so nothing is lost. The counter is a plain increment:
the start-up framework runs driver events one at a time (to be confirmed on the
console with two DualSense pads; see test step 4).

### 2. Basic input (translation)

**Hook.** The existing input redirect stays: transport method 4, slot
`0x37CE38`. `shim_input` runs the original read first, as now.

**Identity.** pad+0x4F0 holds a pointer to the transport object, which has no
pointer back. The wrapper reads the pad manager (`*0x4A1CB8`) and scans its 16
object pointers (manager+0x60) for the non-null object whose +0x4F0 equals the
transport. Pure loads, at most 16 iterations, no lock: the pad being read is
alive because its own input handler is the caller. The found object must then
pass the pad vtable `0x38CDC8` and `pad+0x68 = 054C/0CE6` checks; otherwise
(manager null, no match, another device) the original result is returned
untouched. The plugin's old handle-to-slot table cannot serve as a fallback:
the handle byte is 0 for every Bluetooth pad.

**Translation.** For a DualSense whose read returned report `0x01` of the basic
length, rewrite the buffer in place into the 49-byte DS3 report `0x01` and
return 49. A new pure function, `ds3_report_from_dualsense_basic()`, sits beside
the existing translator and is built both for the host tests and freestanding.
Mapping:
- Buttons, D-pad and sticks direct; stick deadzone from config.
- Create = Select, Options = Start, PS = PS; touchpad click follows
  MAP_TOUCHPAD.
- L2/R2 pressure from the analog triggers; other pressure bytes 0 or 255.
- Motion sensors held at the DS3's centre value until stage 2.

Anything else (`0x31`, other IDs, other lengths, other devices) passes through
unchanged. The existing `0x31` translator stays for stage 2.

### 3. Installer, acceptance and data block

**Redirects** (aligned 8-byte pointer swaps, verified then read back): event
handler `0x37CFF8` -> `shim_event`, enable event handler `0x37CFB0` ->
`shim_enable`, input `0x37CE38` -> `shim_input`. Each slot's original value and
its function's first code qword hash are checked before any write. The
start-up slot `0x37CFE8` is never written.

**Data block, version 3:** magic, version, enable flag, the three original
descriptors, the pad vtable, the pad manager global's address, config
(deadzone, MAP_TOUCHPAD), the info template, counters (completions, which also
numbers the identities; enables; translated; passed).
The connection slots, connection ids, send-routine descriptor, PDU buffer and
start-try counters are removed, and the plugin no longer tracks connections
for the shim.

**Product-ID acceptance, staged:**
- Stage 1 (this spec): pure kernel word edits, the variant proven hang-free on
  the console: helper `0x29E2CC` `xori 0x042F -> 0x0CE6`, and only if plan task 1
  shows it is needed, the inline compare `0x11B5E4` (0x03D5 -> 0x0CE6) plus
  `0x11B5EC` (branch -> `0x11B234`). Side effects while active: a Move
  navigation controller (0x042F) gets no pad object over Bluetooth; with the
  inline edits, a Move motion controller (0x03D5) takes a different branch.
  Both last only while the debug build runs the shim (RAM-only, cleared by a
  reboot).
- Stage 1b (separate small spec, before any release build): restore both Move
  devices by routing the helper and the inline site to the shim; each armed
  and console-tested on its own, which also isolates the earlier hang suspect.

**Install order:** allocate, copy blob and data block, read back; instruction
cache coherency as in spec 1; acceptance word edits; event redirect; enable
redirect; input redirect. Any failure undoes every earlier write in reverse order. Plugin
unload switches the redirects and word edits back; the blob stays allocated.

## Facts to settle read-only first (plan task 1)

Each has a decision rule; anything unsatisfiable means the shim is not
installed. Facts 1-3 and 6 were settled by plan task 1; see "Amendment after
plan task 1" for the outcome, which supersedes the rules for 1-3 below.

1. **The DS3 info template:** read a connected DS3's pad+0x80..+0xD5 on the
   console; determine which fields the kernel reads (the check `0x2A3AD8`, the
   port assignment `0x2A08A4`, anything reading pad+0x88..+0x8F).
   Rule: fields that are read get DS3 values; per-pad fields come from the
   plugin.
2. **Transport embedding:** pad+0x4F0 holds the transport object (vtable
   `0x37CE18`) in the navpid dump. Rule: if not, keep the plugin's
   handle-to-slot table for input identity.
3. **Output on port assignment:** find the DS3 output path (player LEDs) and
   what a refused output report does. Rule: if a refusal can remove the pad,
   add an output wrapper that, for a DualSense, returns success without sending.
4. **Inline acceptance needed?** Decided by console test step 2 (helper edit only).
   Rule: if the pad object forms without it, the inline site is never edited.
5. **Basic report layout and length** as this DualSense sends it, captured
   passively by the plugin in console test step 3 before translation is armed.
   Rule: the translator matches that layout exactly; Mute is mapped only if
   present.
6. **Hook slot values:** `0x37CFF8` holds `0x800000000033A2B0`, and the event
   handler's code hash matches. Rule: mismatch means no install.

## Testing

Host (existing clang harness):
- `shim_event` as a pure decision function over fake driver and pad memory: a
  DS3, a null pad, a wrong vtable, another VID/PID or a disabled shim reach the
  original; a DualSense gets the template copied, its own identity, status 0,
  substate 9, return 0; a second event goes to the original; two DualSense
  completions get different identities.
- `shim_enable` the same way: a DualSense gets status 0 and the done flag, any
  other pad or a done driver reaches the original.
- Input pad lookup over a fake pad manager: found, not found, null manager,
  null entries, a match that is not a DualSense.
- Basic translator: golden tests from recorded reports; fuzz at every length
  0..0x41 (no out-of-bounds; anything but a basic `0x01` passes through).
- Installer against the fake LV2: the new rows, every rollback branch, never a
  write to `0x37CFE8`.
- Packer accepts the new blob.

Console test steps (DEX 4.92, RAM-only, one change at a time; the plugin's in-process
capture observes; webMAN polled at no more than 2-3 requests per second):
1. Install with the DualSense off: redirects verified; a DS3 over USB and
   Bluetooth still works.
2. Event and enable hooks with helper-only acceptance: `CAPTURE_PTR` on the
   pad object shows it staying past 0.9 s with the info block filled, the
   enable driver completing, and a port assigned.
3. Input hook: capture one raw basic report, then check translated input in
   the XMB.
4. DS3 and DualSense together, then two DualSense pads together (different
   identities, both ported); reconnect cycles; plugin unload restores the
   redirects.

## Later stages

- **Stage 1b** (before any release build): restore Move devices (see
  "Product-ID acceptance").
- **Stage 2:** full reports and USB-cable pairing, specified together after
  stage 1 proves a native port.
  - Full reports `0x31` (motion, touchpad), which needs a request sent from the
    kernel code; the earlier hang suspect is re-analysed and tested on its own
    first.
  - USB-cable pairing as designed in spec 1's "Pairing" section (spec 1 plan
    tasks 9-11: pairing facts, `src/usbpair.c`, console stage), so a DualSense
    is paired by plugging it in once, like a DS3. Until then a DualSense is
    registered through the XMB and the debug tooling flips its type to Sony.

## Out of scope

Output (spec 2: rumble, LEDs, light bar), DS4 and DualSense Edge, firmwares
other than table entries.
