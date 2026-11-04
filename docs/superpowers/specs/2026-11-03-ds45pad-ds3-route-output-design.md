# DS3 route, stage 2 piece 1: output (rumble, player LEDs, light bar)

Status: design, awaiting review. Date: 2026-11-03. Firmware: DEX 4.92 only (the
shim is fingerprint-gated, debug VSH builds only, RAM only). Builds on the
start-up completion spec
(`2026-11-01-ds45pad-ds3-route-startup-completion-design.md`, Amendments 1-6).

## Goal

A game or the XMB that sets rumble or LEDs on a DualSense that the PS3 treats as
a DualShock 3 gets the real effect on the DualSense: both motors, the player
LEDs showing the controller number, and the light bar showing a colour per
controller number. A real DS3 and every other device behave exactly as today.

Not in this spec (each is its own spec): full `0x31` input (motion, touchpad,
real battery level), the DS3-format power-off, USB-cable pairing. The shim stays
within its rules: it only rewrites bytes the kernel is already sending, it adds
no send, no lock and no call of its own.

## Why this is needed

The kernel's DS3 pad object builds a DS3 output report and sends it over
Bluetooth. A DualSense ignores or cannot use it, so a DualSense seen as a DS3 has
no rumble and no lights. Stage 1 left the output path untouched on purpose
(Amendment 1, "a refused output report cannot remove the pad").

## Facts

Each is marked **verified** (read in the kernel image or the repo), **sourced**
(from public code, summarised by a research subagent, to be re-read before
coding) or **open** (must be settled in the plan).

Kernel side, DEX 4.92 (LV2 offsets, `0x8000000000000000` + offset):

- **verified.** The DS3 output report is built by `0x2A2C30(pad, request, ...)`
  into the pad object's own buffer at pad+`0x22B` (64 bytes) by the packer
  `0xCC054`, which returns the payload length. It runs only when pad+`0x22A` is
  non-zero. For a Bluetooth pad (pad+`0x6C` == 2) it calls
  `0x10283C(handle = pad+0x20, type 2, report id 1, buffer, length)` at
  `0x2A2DE8`.
- **verified.** `0x10283C` takes the Bluetooth lock, resolves the connection and
  calls the HID output sender `0xF8D60`, which refuses a payload longer than 64
  bytes (`cmplwi ... 0x40` at `0xF8D74`, error `0x80010002`), copies the payload
  into the shared packet buffer `0x4326C8` behind a two-byte header
  (`0xA0 | type`, report id) and hands the packet to the L2CAP sender
  `0x116EF0`. The payload is copied before the call returns.
- **verified.** The pad object is the buffer address minus `0x22B`, so the hook
  can recognise a DualSense pad with the existing `shim_pad_is_dualsense`.
- **verified (plan task 1, `docs/superpowers/notes/2026-11-03-ds3-route-output-facts.md`).**
  The shared packet buffer has room for only **72 bytes**: the object at
  `0x432710` follows it and starts with the pointer to the registered Bluetooth
  HID client, so a 79-byte packet would overwrite it (see Amendment 1). Nothing
  below `0xF8D60` caps the length below 77. The call at `0x2A2DE8` is the only
  path for pad rumble and LED updates over Bluetooth, but not the only Bluetooth
  path that sends a DS3-format output report: the start-up drivers send an
  all-zero one through a control-channel sender with no cap, which the patches
  do not touch.

DualSense side (sourced: Linux `drivers/hid/hid-playstation.c`, third-party
`dualsensectl`):

- Bluetooth output report `0x31`, **78 bytes**: byte 0 report id, byte 1
  sequence in the high nibble, byte 2 tag `0x10`, bytes 3-49 a 47-byte common
  block, bytes 50-73 reserved, bytes 74-77 CRC32 (little-endian).
- CRC: reflected CRC-32 (polynomial `0xEDB88320`, init `0xFFFFFFFF`, final XOR)
  over the byte `0xA2` followed by bytes 0-73.
- Common block offsets in the report: valid_flag0 = 3 (bit0 compatible
  vibration), valid_flag1 = 4 (bit0 mute LED, bit2 light bar colour, bit4 player
  LEDs), motor_right = 5, motor_left = 6, valid_flag2 = 41, light bar setup = 44,
  brightness = 45, player LEDs = 46, light bar R/G/B = 47/48/49.
- Player LED patterns (unconfirmed): P1 `0x04`, P2 `0x0A`, P3 `0x15`, P4 `0x1B`.
- No shorter or smaller output report is known; a short `0x31` is not shown to
  work. **open:** whether the pad accepts output while it still sends the short
  basic input report. Settled on the console (probe stage).

DS3 side (**verified** in the kernel, the packer `0xCC054`): the payload the
kernel hands to the sender is always **48 bytes**, the report id not included.
Payload bytes 1-4 are the rumble request (right duration, right/small motor
on-off, left duration, left/large motor 0-255), byte 9 is the LED bitmap (LED1
`0x02` ... LED4 `0x10`, masked with `0x1E`), bytes 10-29 are four 5-byte LED
timing blocks, the rest zero. In the periodic path the durations are the constant
`0x96` (finite). A real DS3's idle report in the dumps matches. The meaning of
each byte is the standard DS3 one (assumed, from Linux `hid-sony.c`); a capture
with the motors on (observe stage) confirms it.

## Design

### 1. Architecture and data flow

1. **`shim_output`** runs at the Bluetooth send call `0x2A2DE8`. It receives the
   call's arguments, tests for a DualSense pad (buffer minus `0x22B`) and either
   passes the call through unchanged or rewrites it.
2. **`ds3_output_to_dualsense`**, a pure function built twice (host tests and the
   blob, no libc, no static data), translates the DS3 payload into the 78-byte
   report in a buffer in the shim's data block.
3. On a rewrite the hook calls the original `0x10283C` (a tail call) with report
   id `0x31`, the new buffer and payload length 77 (the report without its id).
   The sender copies it at once, so one buffer is enough.
4. Kernel patches at install: the call site becomes a branch-and-link to the
   blob's output entry, the sender's 64-byte cap is raised to 77, and the two TOC
   slots of the shared packet buffer are redirected to a 96-byte buffer of ours
   (Amendment 1).

### 2. Translation

- Header `0x31`, sequence byte `seq << 4` from a counter in the data block that
  wraps at 16, tag `0x10`. Bytes 50-73 are zero. CRC as above, computed with a
  bitwise loop (no table).
- Valid flags are set on every report (full state each time, no tracking):
  flag0 bit0, flag1 bits 2 and 4, and the light bar setup: valid_flag2 (byte 41)
  = `0x02` (light bar setup control enable) and light bar setup (byte 44) =
  `0x02` (fade light out). A DualSense plays a Bluetooth start-up animation on
  its light bar until the host sends the setup, and ignores the colour meanwhile
  (the Linux driver's `dualsense_reset_leds` does the same before programming
  the colours); the setup is sent with every report, so it stays stateless.
- Large motor 0-255 is copied to motor_left. The small on/off motor becomes
  motor_right 255 or 0.
- The DS3 LED bitmap gives the player number (lowest set bit among LED1-LED4):
  the player LEDs show it and the light bar takes the colour for that number:
  P1 blue, P2 red, P3 green, P4 pink, in one constant table. No LED bit set:
  player LEDs and light bar off. Timing blocks are ignored (steady).
- Rumble durations: if the capture shows the kernel always sends "forever", the
  translation is stateless. If it sends finite durations, the plan must decide
  (probably a short stop timer, which needs state) before the full stage.
- Any report that is too short, has an unexpected id or does not look like the
  captured layout is **passed through unchanged**. The hook never blocks a send.

### 3. Install and safety

- **Call site patch:** `bl 0x10283C` at `0x2A2DE8` becomes a branch-and-link to
  the output entry in the blob. This is a new kpatch kind (BRANCHL) beside WORD
  and BRANCH. The entry uses the same framed-wrapper technique as the three
  existing hooks: adopt the blob TOC, restore the kernel `r2` before the tail
  call, leave the link register alone. The plan must show this works for a `bl`
  rather than a descriptor call.
- **Cap patch:** the compare at `0xF8D74` is raised from `0x40` to `0x4D` (77).
- Both rows are verified by the FNV of the 8-aligned qword, applied in the same
  all-or-nothing transaction as the existing hooks, reverted in reverse on
  unload, RAM only, DEX 4.92 fingerprint only. A mismatch at any output site
  **fails the whole install** (fail closed, as for the current hooks).
- Config key `SHIM_OUTPUT_HOOK` (debug builds; default on) switches the output
  patches off. A second key `SHIM_OUTPUT_MODE` selects `0` observe (hook
  installed, everything passes through, the first payloads are traced), `1` probe
  (fixed light bar colour) or `2` full. Only the full mode is the product
  behaviour; the other two exist for the staged console runs and may be removed
  once the full mode is proven. The default is `0` (observe) while the stages are
  being run, and the change that proves the full mode also switches the default
  to `2`. In observe mode the cap patch is not applied, so nothing the kernel
  sends changes.
- `DS45_SHIM_VERSION` 8: the data block gains the 80-byte output buffer, the
  sequence counter and the debug counters (out calls, translated, passed, last
  length, captured payload bytes), so an older install is never adopted. The
  `shimdbg` trace line gains the output counters.

### 4. Testing

Host tests, written before the code they cover:

- Translator: golden bytes for header, flags, motors, LEDs and colour per port,
  padding; the CRC against an independent reference loop in the test (`0xA2`
  prefix); sequence wrap; short and wrong-id reports decline; every input length
  with guard bytes after the input, never reading past the end; a fuzz run.
- Hook: a DualSense pad gets the rewritten arguments and a tail call; a DS3, a
  non-Sony pad, a missing config and `SHIM_OUTPUT_HOOK = 0` pass through byte for
  byte; counters.
- Patcher and installer: BRANCHL encoding, range and hash; both rows verified,
  applied, reverted; a mismatch at either site fails with nothing written; data
  block version 8; adoption; the built-in row against the pristine image when it
  is present.
- Packer: the new entry is exported and obeys the packer's rules.

Console (DEX 4.92), three stages, each stopped if any pad misbehaves:

1. **Observe.** Hook installed, everything passes through, the first DS3 output
   payloads are traced. DS3 and DualSense behave exactly as before. Gives the
   layout and the duration answer.
2. **Probe.** A fixed light bar colour to one DualSense with the cap raised.
   Answers whether the pad accepts output in basic input mode and whether the
   79-byte packet is fine.
3. **Full.** Real rumble (the XMB vibration test or a game), player LEDs and
   colour by port with a DS3 and two DualSense, reconnect.

Success: each DualSense lights its port's colour and player LEDs; rumble works
and stops; the DS3's own rumble and LEDs are untouched; no freeze, no dropped pad
and no change to controller numbering across reconnects.

## Risks and open questions

- The pad may refuse output while in basic input mode. If so, full-report input
  (the other stage 2 piece) has to come first, and this spec is resumed after it.
- (Resolved by Amendment 1.) The shared packet buffer could not hold 79 bytes;
  the design now moves it.
- A branch-and-link entry is a new kind of patch; a wrong TOC or `r2` there is
  the same class of fault as the earlier hangs. Hence the observe stage first.
- The player LED patterns and colours are chosen by eye on the console.
- Finite rumble durations (see Translation).

## Amendment 1 (2026-11-03): the packet buffer is moved

Plan task 1 found that the shared packet buffer `0x4326C8` is 72 bytes, and the
79-byte packet would overwrite the first qword of the HID client state at
`0x432710` (a live pointer, null only while nobody is registered). The sender
cap alone is therefore not enough.

**Decision.** Every user of the buffer reaches it through one of two TOC slots:
the qword at `0x372650` (the buffer, `0x80000000004326C8`) and the qword at
`0x372660` (the buffer plus 1). The installer redirects them with two pointer
patches (the kind already in the patcher) to a **shim-owned packet buffer of
96 bytes** in the blob's data block, the second slot to that address plus 1.
All seven load sites then use it; their own use never exceeds 66 bytes (65 for
receive), so they stay correct. No call is needed, so the shim rules hold. The
facts note lists the sites and states that no other qword or address
construction points into the old buffer.

**Consequences.**
- Patches in total: the call site (branch-and-link), the two buffer slots, and
  the cap word. The transaction grows from at most 6 to at most 10 entries; the
  patcher's list limit and its compile-time checks are raised to match.
- Staging changes: observe mode (`SHIM_OUTPUT_MODE = 0`) installs the call site
  **and the buffer redirect**, so the moved buffer is proven harmless with normal
  traffic (pad input must keep working, since the receive paths use it) before
  any longer packet exists. Probe and full mode add the cap word.
- The cap raise also lets the other caller of the sender (a generic method) send
  up to 77 payload bytes; with the 96-byte buffer that is safe.
- A report longer than the transmit ring has room for (134 bytes on the
  interrupt channel; 81 are needed) is dropped by the kernel (`0x1169E4`
  returns 0); the next periodic update resends, so a drop is harmless.
- The two slots are patched one after the other. A receive running between them
  could pair the old buffer with the new "plus 1" slot once. The shim installs at
  boot before any pad connects, which is why this is accepted; the observe stage
  checks pad input afterwards.
- The receive side keeps its 65-byte limit; raising it belongs to the
  full-report input piece, not this one.
- Rumble: the kernel's durations are finite (`0x96`), and a DualSense keeps
  vibrating until told to stop. Whether the kernel sends a zero-motor report when
  rumble ends, or relies on the DS3 expiring, decides the translation. The
  observe stage captures a rumble start and stop; a stateless translation that
  would leave a DualSense vibrating is not acceptable, and the plan must then
  decide before the full stage.

## Amendment 2 (2026-11-03): output needs the full-report input

The console probe (facts note F8) showed that a DualSense accepts the rewritten
78-byte output report while still in its basic input mode (light bar turned
blue, no hang), but that its **input then stops**: translated reads stay at 1 and
the reads seen on its transport crawl. The likely cause is that the pad answers
its first output report by switching to the full 78-byte `0x31` input report
(79 bytes with the header), and this kernel's HID receive functions read at most
`0x41` (65) bytes and refuse a longer packet, leaving it queued.

**Decision (user, 2026-11-03).** This piece now includes receive-side support for
the full report, so a DualSense keeps working after the first output report. The
input translator for the `0x31` report already exists (`ds3_report_from_dualsense`
reads through the status byte, 55 bytes, and also gives motion and the real
battery level). What is missing is the path that gets the 79-byte packet to it.

**Shape of the work (settled by a facts task before any code).**
- The receive read limit: the four `0x41` immediates in the HID receive
  functions (facts F1.1), the interrupt-in ring capacity, and what the receive
  function does with the packet (how it copies into the pad's buffer and whether
  that copy is bounded by the caller's length). The moved packet buffer (96 bytes)
  already holds a 79-byte packet.
- The input hook: its `rc > 0x41` guard and the minimum buffer length, and what a
  full report looks like once delivered (length, buffer capacity, truncation).
- The patches are WORD rows like the cap word (verified, journalled, reverted on
  unload) and are applied together with the output sites; their gating and the
  staged console runs follow the same pattern (probe first, then full).
- Risk: raising a shared receive limit touches every Bluetooth HID device, so the
  facts task must show the effect on the DS3 (50-byte reports) and on anything
  else that shares the functions, and the console stages must show the DS3
  unchanged. A 79-byte packet that the copy into the pad's buffer cannot bound
  would be an overflow; the facts task states this either way and the design
  changes if it is.

**Success criteria added.** After the first output report the DualSense's input
keeps working (sticks, buttons, motion, battery), with the DS3 unchanged.

## Amendment 3 (2026-11-03): receiving the full report through a bounce buffer

Plan task 11 (facts note F9) found that the kernel's interrupt-in receive
function copies a received packet into the caller's buffer without checking its
size, and the pad driver's buffer is a 64-byte stack local. The read limit
(`0x41`) is also what keeps a 79-byte full report out: an over-long record has
its 2-byte length prefix consumed and then the read fails with `0x80010028`, so
the ring is left out of step and no report is ever delivered. Raising the limit
alone would overflow the stack buffer by about 14 bytes.

**Decision (user, 2026-11-03).**
- Raise the interrupt-in read limit from `0x41` to `0x50` with three WORD rows
  (`li r7,0x50` at `0xF9094`, `0xF9134`, `0xF91F4`; channel 1 only, the
  channel-3 limit at `0xF8484` stays). They are verified, journalled and
  reverted like the other rows, and applied only when the output hook rewrites
  (`SHIM_OUTPUT_MODE` 1 or 2), because only a rewritten output report makes the
  pad send the long report.
- The input hook (`shim_input`), in those modes (1 and 2, shim enabled, transport
  and destination non-NULL), bounces EVERY read, whichever pad it is: it calls
  the original read with a shim-owned **bounce buffer** (`inbuf`, 96 bytes, in
  the data block) and the larger length, then looks the pad up. Only a
  DualSense's report is translated, there; for every other pad (the DS3
  included, and a pad the lookup misses) the bytes are passed through
  unchanged. Either way at most the caller's `len_in` bytes are copied into the
  caller's buffer. A result that is not translatable and does not fit is
  dropped (the facts task states what the callers do with a zero return). The
  reason for bouncing every read: the raised limit is shared by the whole
  channel, and a read whose pad lookup misses (or whose pad manager or class is
  not yet known) must not overrun the caller's 64-byte buffer either, so the
  decision which pad it is cannot be taken before the read. Mode 0, any other
  mode value, a disabled shim and NULL arguments read exactly as today, into the
  caller's buffer.
- The input filter and the translator are unchanged: they already translate a
  `0x31` report (motion, touchpad flags, battery level), so those come with it.

**Risks.**
- The limit raise is shared by every device on that channel. A device that sends
  more than 64 bytes was already stalling; after the patch its reports are
  delivered into callers that cannot hold them. The facts task enumerates the
  other callers of the receive function before any code is written, and the
  design is reopened if one can hold a device that sends long reports.
- The patch transaction grows to 13 entries; the patcher's limit is raised to 16.
- The bounce path first runs on the console in the probe stage; the DS3 must stay
  unchanged throughout.

## Plan outline (to be expanded by the writing-plans skill)

1. Facts (read-only): buffer ownership and length limits below `0xF8D60`; the
   exact DS3 payload and every caller of the BT output wrapper; the entry-stub
   calling contract for a `bl`; the hash rows for the two sites.
2. Host: translator, tests first.
3. Host: BRANCHL kind, the installer rows, data block v8, the hook, config keys.
4. Console: observe, then probe, then full; each with the restore-to-stock
   procedure from `tools/ps3_console_restore.ps1`.

## Sources

- Linux `drivers/hid/hid-playstation.c` (DualSense report 0x31, CRC, flags).
- Linux `drivers/hid/hid-sony.c` (DualShock 3 output report).
- `dualsensectl` `main.c` (third party; the Bluetooth power-off feature report,
  for a later spec).
- Kernel facts: this repo's notes under `docs/superpowers/notes/`.
