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
- **open (plan task 1).** Whether the memory after the shared packet buffer
  (`0x4326C8` + 79 bytes = `0x432717`) is unused; whether `0x116EF0` or anything
  below it has its own length limit; the exact DS3 payload the kernel builds
  (length, offsets, duration bytes); that every Bluetooth DS3 output goes through
  the call at `0x2A2DE8`.

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

DS3 side (sourced: Linux `hid-sony.c`): 36-byte report with the rumble bytes at
1-5 (right duration, right motor on/off, left duration, left motor 0-255) and the
LED bitmap at byte 10 (LED1 `0x02` ... LED4 `0x10`). The PS3 may use a longer
form: the translator's offsets come from a capture of the report the kernel
really builds, not from this table alone.

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
4. Two kernel patches at install: the call site becomes a branch-and-link to the
   blob's output entry, and the sender's 64-byte cap is raised to 77.

### 2. Translation

- Header `0x31`, sequence byte `seq << 4` from a counter in the data block that
  wraps at 16, tag `0x10`. Bytes 50-73 are zero. CRC as above, computed with a
  bitwise loop (no table).
- Valid flags are set on every report (full state each time, no tracking):
  flag0 bit0, flag1 bits 2 and 4.
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
  unload, RAM only, DEX 4.92 fingerprint only. A mismatch at either site **fails
  the whole install** (fail closed, as for the current hooks).
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
- The shared packet buffer may not have room for 79 bytes, or a lower layer may
  cap the length. Then approach A as written cannot ship and the design is
  reopened (candidates: a smaller report if one is found, or moving the packet to
  a buffer of our own, which needs a call and is outside the shim rules).
- A branch-and-link entry is a new kind of patch; a wrong TOC or `r2` there is
  the same class of fault as the earlier hangs. Hence the observe stage first.
- The player LED patterns and colours are chosen by eye on the console.
- Finite rumble durations (see Translation).

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
