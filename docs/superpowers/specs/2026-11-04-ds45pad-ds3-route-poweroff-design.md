# DS3 route, stage 2 piece 2: power-off ("Turn off the controller")

Status: design, awaiting review (2026-11-04). Firmware: DEX 4.92 only (the shim
is fingerprint-gated, debug VSH builds only, RAM only). Builds on the output spec
(`2026-11-03-ds45pad-ds3-route-output-design.md`, merged as PR #5) and uses the
same hook technique, patcher and install rules.

Kernel facts: `docs/superpowers/notes/2026-11-04-ds3-route-poweroff-facts.md`
(P1-P7). Static analysis only; nothing in it was seen on a console yet.

## Intent

What the user said: "Turn off the controller" on a real DualShock 3 powers it
off; on a DualSense run through the DS3 route it only disconnects it, and the
DualSense stays a worse pad than a DS3 in that one respect. The project standard
(memory: native DS3 standard) is that a DualSense must work like a DS3 or
better, with no prompt, no confirmation and no extra step for the user.

Assumptions made here (correct me if any is wrong):

- "Turn off the controller" means the XMB / PS-button-menu action on one pad. The
  same kernel request may also fire at other moments where a DS3 would be
  commanded off (for example when the console shuts down); a DualSense should then
  power off exactly as a DS3 does. This spec does not add any new trigger.
- A powered-off DualSense comes back with the PS button, with the same controller
  number as before (stage 1b stable identity), exactly like a DS3.

## Goal

When the PS3 asks a Bluetooth DualSense (presented as a DS3) to turn off, the
DualSense powers off. A real DS3 and every other device behave exactly as today.

Not in this spec: USB-cable pairing, anything that runs when the user holds the
PS button on the pad itself (the DualSense already powers itself off then), auto
power-off timers, and a link-teardown fallback (Risks: only designed if the
console stages show the report approach cannot work).

## Facts

Each is marked **[code]** (read in the kernel image), **[inferred]**, **[assumed]**
(device behaviour) or **sourced** (public code, summarised by a research subagent,
to be re-read from the primary source before coding) or **open** (settled by a
console stage).

Kernel (LV2 offsets, `0x8000000000000000` + offset; details and hash rows in the
facts note):

- **[code]** The kernel never builds a Bluetooth HID_CONTROL message, so
  VIRTUAL_CABLE_UNPLUG is never sent. A real DS3 is therefore not switched off by
  that message. Exactly three packet builders exist: DATA output (`0xF8D60`),
  SET_REPORT (`0xF8BD8`, control channel, header `0x50` OR type) and GET_REPORT
  (`0xF8E98`).
- **[code]** The privileged pad-manager ioctl `0x2A1B90`, command 6, calls
  `0x2A19D4`, which calls the pad's vtable `+0x98` = `0x2A3280`. For a Bluetooth
  pad that allocates a command object (vtable `0x37CEF0`, kind 3) whose run method
  `0xCA90C` builds the 4-byte payload `42 08 00 00` and sends it as a feature
  SET_REPORT with report id `0xF4` through the wrapper `0x102750`: the call is
  `bl 0x102750` at `0xCA9BC`, registers `r3` handle byte, `r4` = 3, `r5` = `0xF4`,
  `r6` payload buffer (`obj+0x54`), `r7` = 4, `r8` cookie, `r9` the object (kind
  `obj+0x38`, pad `obj+0x48`). The wire packet is `53 F4 42 08 00 00`. Afterwards
  `0x2A0300` unassigns the port and publishes the "port removed" event. No
  disconnect is issued anywhere in this chain.
- **[inferred]** This command-6 chain is the pad-side "Turn off the controller"
  request. It is the only pad-side request that tells the controller something
  and releases the port; which syscall the XMB uses to reach it was not found.
- **[code]** Nothing in the chain looks at VID/PID except two gates the shim
  already satisfies (firmware word `pad+0x82` > `0x114`, bus type 2) and the
  VID/PID helper `0x29E2B4`, already patched to accept `0x0CE6`. A DualSense seen
  as a DS3 therefore already receives `F4 42 08 00 00` today and has no reason to
  react to it.
- **[code]** The kernel's link-teardown entries are `0x1174FC(address, flag)` and
  the HCI emitter `0x112BE0` (opcode `0x0406`). No pad-class code calls them. The
  call sites reachable from user requests are `0xF4D94` (Bluetooth control
  syscall; that command also deletes the stored pairing), `0xF722C` (inside
  `0xF71E4`, reached through `0x10268C`) and `0xFDB44` (HID client vtable `+0x68`).
- **[code]** The control-channel sender `0xF8BD8` has no 64-byte cap and copies the
  payload before it returns; the control-channel transmit ring holds 67 bytes.
  A 46-byte payload (48 bytes on the wire) fits both.
- **[code]** The output hook at `0x2A2DE8` cannot see this message (different
  sender and wrapper), so the existing hook is no help; a new site is needed.

DualSense side (sourced: third-party `dualsensectl`, summarised by a research
subagent; **open** until a console shows it works):

- Bluetooth feature report id `0x08`, 47 bytes in total (id included): byte 0 =
  `0x08`, byte 1 = `0x02`, bytes 2-42 zero, bytes 43-46 = CRC-32 (little-endian).
  The CRC is the same reflected CRC-32 the output report uses (polynomial
  `0xEDB88320`, init and final XOR `0xFFFFFFFF`) but over the seed byte **`0x53`**
  followed by bytes 0-42. `0x53` is exactly the header the kernel puts in front of
  a type-3 SET_REPORT (`0x50` OR 3), which is consistent with that description.
- In the kernel's terms (report id separate from the payload) this is report id
  `0x08` and a **46-byte payload**: `02`, 41 zero bytes, the 4 CRC bytes.
- Whether the pad answers with a success handshake, whether it powers off before
  the kernel's retry logic runs again, and whether the report really powers a
  Bluetooth DualSense off (rather than only the USB or some firmware variants) are
  all **open**.

Device side, DS3 (**[assumed]**): a real DS3 powers itself off on, or after,
`F4 42 08 00 00` (all enable bits clear). Not needed by the design; the observe
stage records what the console does for a DS3 as the baseline.

## Design

### 1. The hook

A hook at the shared send call `bl 0x102750` at `0xCA9BC` (qword `0xCA9B8`,
FNV-1a 64 `0x6D3B5A2728211E68`), entered by a plain `bl` exactly like the
existing output hook: a framed asm entry in `shim/entry.c` adopts the blob TOC,
saves the argument registers `r3`..`r9`, calls a C body, restores the kernel `r2`,
LR and the arguments, and tail-calls the original `0x102750` (its code address in
a new data-block field `orig_poweroff`). The C body may change only `r5` (report
id), `r6` (payload pointer) and `r7` (length); every other register reaches the
kernel unchanged, so cookie, context, retries and callbacks behave as today. The
hook holds no lock and calls nothing in the kernel; the Bluetooth lock is taken
inside `0x102750` as before.

The C body, `shim_poweroff`, rewrites the call only when **all** hold: the hook
is on and the mode is act; `r4` == 3, `r5` == `0xF4`, `r7` == 4; the object's kind
(`obj+0x38`) == 3; and the pad at `obj+0x48` is a DualSense (the existing
`shim_pad_is_dualsense`). Otherwise it leaves the registers alone and the call
goes through unchanged. It never blocks or fails a send.

On a rewrite it builds the 46-byte payload in a new 48-byte buffer `po_buf` in
the data block (the shim's own memory, so the kernel's command object is never
modified; the sender copies the payload at once), and sets `r5` = `0x08`, `r6` =
`po_buf`, `r7` = 46. A pure function `dualsense_poweroff_payload(uint8_t out[46])`,
built twice (host tests and the blob, no libc, no static data), fills it; the
existing CRC code in `shim/ds3_output.c` is generalised to take the seed byte
(`0xA2` for the output report, `0x53` here).

### 2. Observe mode and the teardown observers

`SHIM_POWEROFF_MODE` selects `0` observe or `1` act. In both modes the `0xCA9BC`
hook counts calls and records an event (below) but in observe mode it changes
nothing. In **observe mode only**, three more observer patches are installed, one
at each user-reachable teardown call site (`0xF4D94`, `0xF722C`, `0xFDB44`). They
share one entry `shim_teardown_obs` that records an event and tail-calls
`0x1174FC` unchanged; `0xF722C` is a tail `b` (a BRANCH row), the other two are
`bl` (BRANCHL rows). The entry preserves LR, so a `bl` and a `b` both work.
Their only purpose is to answer, with the console, whether the VSH disconnects
the link as well as issuing the `F4` request.

Event ring in the data block: 8 records of 16 bytes (sequence number, site id,
DualSense flag, and the site's arguments: for the `0xCA9BC` site handle, `r4`,
report id, length, kind and the first four payload bytes; for the teardown sites
the flag, the low 32 bits of the address and the low 32 bits of LR) plus counters
(F4 site calls, rewritten, passed through; teardown calls). The `shimdbg` trace
line gains the counters and the ring is read back with the existing console
trace tooling (`tools/ps3_console_trace.ps1`), extended as needed. No output or
prompt on the console; reading is passive and after the action.

### 3. Install and safety

- New patch rows, in the same all-or-nothing, journalled transaction as the other
  hooks, verified by the FNV of the 8-aligned qword, RAM only, DEX 4.92 only: [0]
  the `0xCA9BC` call (BRANCHL, aimed at `shim_poweroff`) in both modes; [1]-[3]
  the teardown observers, only in observe mode. A mismatch at any applied site
  **fails the whole install** (fail closed, as for the current hooks).
- Applied in the same position as the output sites (before the event, enable
  and input redirects, reverted after them at unload). Recorded in the data
  block's applied set so an adopted install can undo them.
- `DS45_KPATCH_MAX` goes from 16 to 20: the current worst case is 13
  entries and observe mode adds four. The compile-time assertions that pin the
  count (`kshim_final_is_thirteen`, `kshim_pid_sites_match`,
  `kshim_adopt_fits_journal`) are updated. `DS45_SHIM_PID_SITES` goes from 10
  to 14. `DS45_SHIM_VERSION` 12: new fields `orig_poweroff`, `po_buf`, counters
  and the ring, so an older install is never adopted.
- Config keys (debug builds): `SHIM_POWEROFF_HOOK` (0/1, default on) and
  `SHIM_POWEROFF_MODE` (0 observe, 1 act). The mode defaults to **0 while the
  stages are run**; the change that proves the act mode also switches the default
  to 1, as was done for the output mode. With `SHIM_POWEROFF_HOOK` = 0, none of
  the new sites is installed.
- The hook installs independently of the output and input hooks: it needs
  nothing from the packet-buffer move (a 48-byte packet fits the kernel's own
  72-byte buffer, and fits the moved 96-byte one).

### 4. Testing

Host tests, written before the code they cover:

- `dualsense_poweroff_payload`: golden bytes (46 bytes, `02` then 41 zeros then
  the CRC), the CRC against an independent reference loop in the test (seed
  `0x53` followed by `08` and the first 42 payload bytes); the generalised CRC
  still produces the output report's golden bytes unchanged.
- Hook: a DualSense pad with the exact argument set gets `r5`/`r6`/`r7` rewritten
  and the original called; a DS3, a non-Sony pad, kind != 3, `r4` != 3, `r5` !=
  `0xF4`, `r7` != 4, a null pad, observe mode, hook off and a missing config all
  pass through with every register byte for byte; counters and ring records.
- Patcher and installer: BRANCHL and BRANCH rows for the new sites; observe mode
  applies four sites and act mode one; a mismatch at any applied site fails with
  nothing written; the journal budget; data block version 12; adoption and unload
  revert the new sites; the built-in rows against the pristine image when it is
  present.
- Packer: the new entries are exported and obey the packer's rules.

Console (DEX 4.92), three stages, each ending with `tools/ps3_console_restore.ps1`
and a check that the hooks are original; stopped if any pad misbehaves. Standing
rules as in earlier stages: arm DualSenses by Bluetooth address only, never touch
the Move motion controller's link key, no hard reset, all verification passive.

1. **Observe** (mode 0). With a DS3 and a DualSense, the user chooses "Turn off the
   controller" for the DS3, then for the DualSense, one at a time. Read the ring.
   The question and the gate:
   - A: the `0xCA9BC` site fires for both (kind 3, `F4`, `42 08 00 00`) and no
     teardown site fires: proceed to stage 2 as designed.
   - B: the `0xCA9BC` site fires and a teardown site fires as well, for the DS3
     too: proceed, and record the order; the report must reach the pad before the
     link drops (stage 2 shows whether it does).
   - C: the `0xCA9BC` site does not fire, or only a teardown site does: this spec's
     hook is the wrong site. Stop; write an amendment from what fired.
2. **Act** (mode 1). A DualSense and the DS3: "Turn off the controller" on the
   DualSense powers it off (light bar goes out, no link), the DS3 still behaves
   as in stage 1, and the PS button brings the DualSense back with the same
   controller number and working input and output.
3. **Final check** of the reviewed build with two DualSense and the DS3: each pad
   turned off in turn, then reconnected; the colour and player LED of each pad
   still right.

Success: each DualSense powers off when a DS3 would, comes back with the PS
button and the same number; no freeze, no dropped pad, no change to a DS3 or any
other device.

## Risks and open questions

- The XMB action may not reach the `0xCA9BC` site (P7). Settled by stage 1; the
  teardown observers show which site does fire.
- The `0x08` report is a third-party summary, unverified on a Bluetooth DualSense.
  Plan task 1 re-reads it from the primary source (report id, length, whether it
  is a feature or an output report; an output report would need a different
  hook site) before any code. If stage 2 shows the pad does not power off, the
  next step is a new amendment (candidate: the graceful link teardown
  `0x1174FC` for a DualSense only), not a silent extension of this one.
- Ordering: if a teardown follows the `F4` request closely, the link may drop
  before the pad processes the report. The packets of one link are ordered, but
  the pad's firmware order is not known. Settled by stage 2.
- The pad may answer the report with an error handshake; the kernel then retries
  up to five times, 20 ms apart, re-running the run method and so re-entering the
  hook with identical arguments. Harmless (idempotent) and the same retry count a
  DualSense gets today for the refused `F4`.
- If the same kernel request fires at moments beyond the user's menu action
  (console shutdown, user switch), the DualSense is powered off there too. That
  is the DS3's behaviour and the intent; stage 1 records when it fires.
- The kernel's command object and the shared packet buffer are not modified; the
  only shim state touched is `po_buf` and the ring, written by the hook. The busy
  byte is per pad, so two pads could be turned off at once; the payload is
  constant, so both would write identical bytes into `po_buf` and a torn read
  cannot differ. The ring is a debug aid; a lost or mixed record is tolerated.
- A branch-and-link entry inside a function with its own frame, with the
  arguments passed on in registers, is the same class of fault as the earlier
  hangs; hence observe first, and the register-preservation tests above.
- Plan task 1 is a verification gate, as for the output feature: re-check every
  register, offset and hash named above against the pristine image and the dump,
  confirm that the retry path re-runs `0xCA90C` (so a rewritten call is rebuilt
  each time) and that the argument registers survive from the hook entry to the
  tail call, and confirm the data block and patch counts, before any feature code.
