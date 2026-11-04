# DS3-route output facts - DEX 4.92 (read-only research, plan task 1)

Scope: the facts the stage 2 piece 1 output shim (spec
`docs/superpowers/specs/2026-11-03-ds45pad-ds3-route-output-design.md`) needs about the Bluetooth send path of the
console's DS3 output report. Static analysis only, of the pristine kernel code image and memory dumps of the user's
own DEX 4.92 console. Nothing was sent to any console, no file other than this note was changed.

Sources: `analysis/lv2_code_492.bin` (pristine kernel code, 3 MiB, LV2 offset = file offset) and the RAM dumps in
`tools/dumps/dex492/` (git-ignored, never committed): `ds45pad_lv2.bin` (code identical to the pristine image except
six words at `0x2FE758`, unrelated), `ds3route/navpid_connected.bin` and `ds3route/type01_connected.bin` (a DS3 over
Bluetooth at pad `0x661000`, a DualSense at pad `0x650800` in the first), `twodualsense_snap/*`, `twopads/*`,
`reboot_test/*`, `nopad/*`, `shimtest/*`.

Conventions: a bare `0xNNNN` is an LV2 offset (kernel address = `0x8000000000000000` + offset); `pad+0xNN` is a byte
offset in the pad object (vtable `0x38CDC8`); `TOC-0xNNNN` is a slot relative to the kernel TOC `0x3759C0`. Hashes
are FNV-1a 64 of the 8-aligned qword, bytes in memory order. No kernel code bytes are reproduced: instructions are
described in prose, apart from the single instruction words that the patches need (sections F5 and F9). Each fact is
marked **[code]** (read in the kernel image), **[dump]** (read in a RAM dump) or **[assumed]**.

## GATE: FAILED - the shared packet buffer has room for 72 bytes, the design needs 79

**Approach A as written in the spec cannot ship. The design must be reopened.** The shared packet buffer `0x4326C8`
is followed, 72 bytes later, by another kernel object at `0x432710`. A rewritten report needs 79 bytes (2 header bytes
plus the 77-byte payload), so the last 7 bytes (`0x432710..0x432716`) would land in the first qword of that object.
That qword is a live pointer slot of the Bluetooth HID host client state (F1.3). The lower layers do **not** cap the
length below 77 (F2), so this is the only blocker found.

| Question | Answer |
|---|---|
| Room in `0x4326C8` for 79 bytes? | **No.** 72 bytes up to the next object (66 are used today by a 64-byte payload plus header; 6 bytes are unused padding). Overrun: 7 bytes. |
| Length cap below 77 in `0x116EF0` and below? | **No** (F2). The only cap is the compare at `0xF8D74`. |

## Summary

| Fact | Result | Effect on the plan |
|---|---|---|
| F1 shared buffer | `0x4326C8` is the shared HID packet buffer, used by 5 code sites (3 receive, 2 send) plus 2 sites through the `buffer+1` slot. Nothing else occupies `0x4326C8..0x43270F`, but the object at `0x432710` starts right after and its first qword is a live pointer. | **STOP condition.** 79 bytes do not fit. |
| F2 length limits | None below 77 found. `0xF8D74` caps at 64; below it only "length > 0" and ring free space (interrupt TX ring 134 bytes, HCI ring 2048 bytes). | The cap patch alone would work if the buffer were large enough. |
| F3 callers | `0x10283C`: one caller (`0x2A2DE8`). `0xF8D60`: two (`0x1028C4` inside `0x10283C`, `0x100648` inside a generic method). `0x2A2C30`: three. **The call at `0x2A2DE8` is the only path for pad-object rumble and LED output, but NOT the only Bluetooth path that sends a DS3-format output report**: the start-up drivers send a zero report through `0x102750` (a different sender, `0xF8BD8`). | Unaffected by the patches; listed so the console runs expect them. |
| F4 DS3 payload | 48 bytes (`0x30`), standard DS3 layout without the report id: rumble payload bytes 1-4, LED bitmap at payload byte 9, four 5-byte LED blocks at 10-29, zero to 47. A real DS3's idle report was read from a dump. | Offsets for the translator. Durations are finite (`0x96`) in the periodic path. |
| F5 hashes | Output call qword `0x2A2DE8`: `0x2A29E6BB7BA10D5E`. Cap qword `0xF8D70`: `0xBF27C253F5228E13`, original compare word `0x2B9F0040`, new word `0x2B9F004D`, patched qword `0xBF27CD53F522A0C4`. Three independent computations agree. | Rows for the installer. |
| F6 stub contract | `r3` = pad+0x20 byte, `r4` = 2, `r5` = 1, `r6` = pad+0x22B, `r7` = 0x30, `r2` = kernel TOC, `r28..r31` live, `r3` afterwards is stored as the function's return value. | Contract for the `bl` entry. |
| F9 receive path (task 11) | **GATE FAILED for a bare limit raise.** The HID receive function copies `read - 1` bytes into the caller's `dst` with no bound by `len_in`; the pad driver's `dst` is a 64-byte stack local (72 bytes in one caller) followed by saved registers, so a 78-byte copy overruns the frame. The ring (134 bytes) holds the 81-byte record and the over-long record is NOT merely left queued: the failed read swallows the length prefix and desynchronises the ring. | The three `li r7,0x41` immediates of the interrupt-in channel can be raised only together with a bounce buffer in the input hook (the original is called with a shim-owned buffer of 80+ bytes, the result copied into `dst` bounded by `len_in`). See F9.7. User decision needed. |
| F10 other receive callers and the transport read (task 12) | **GATE PASSED, on two device assumptions** (Move report is 49 bytes; a DualSense of stored type other than `0x01` stays in the basic mode). The only other caller of `0xF8FE0` is the generic HID client (`0x100130`, `0x1007B8`; heap staging buffer, 58 bytes of room), which the DS3 and a type-`0x01` DualSense never use. The original read is safe with a bounce `dst` and `len_in` `0x50`; a 0 return is harmless; the limit is used in one compare; the "crawl" of `inds` is most likely the counter skipping error reads. | Recommend bouncing every transport read, not only the DualSense's; the three limit rows need the moved buffer. See F10. |

## F1 - The shared packet buffer `0x4326C8`

### F1.1 Every user [code]

The buffer address is the TOC slot `TOC-0x3370` (qword at `0x372650`, value `0x80000000004326C8`). A second slot,
`TOC-0x3360` (qword at `0x372660`), holds `buffer+1`. No other qword anywhere in the 8 MiB dump points into
`0x4326C8..0x432717`, and no `lis`/`addi` pair builds such an address in the code. Loads of the two slots, over the
whole code image:

| Site | Function (all in the HID layer) | Use |
|---|---|---|
| `0xF845C` | `0xF840C` | Receive, channel 3 (control, in): clears 65 bytes, reads up to `0x41` bytes, then copies `buffer+1` (via the `TOC-0x3360` load at `0xF8520`) minus the header byte to a callback target. |
| `0xF8C70` | `0xF8BD8` | **Send, channel 2 (control, out)**, header = type OR `0x50` (SET_REPORT), report id, payload copy (16-bit length, no cap of its own), then `0x116EF0`. Used by the start-up drivers via `0x102750`. |
| `0xF8DD8` | `0xF8D60` | **Send, channel 0 (interrupt, out)**: the DS3 output path. Clears 65 bytes, header `0xA0 OR type`, report id, payload copy, then tail-calls `0x116EF0` with length+2. |
| `0xF906C` | `0xF9060` | Receive, channel 1 (interrupt, in): clears 65 bytes, reads up to `0x41`, copies `buffer+1` (via the `TOC-0x3360` load at `0xF91A0`). |
| `0xF91D8` | `0xF91C0` | Receive, channel 1: two reads of up to `0x41` bytes (drain), then clears 65 bytes. |

The channel roles (control or interrupt, in or out) are inferred from the header byte (`0x5x` SET_REPORT on a control
channel, `0xAx` DATA on an interrupt channel) and from the ring sizes (F2); the code itself only uses the index 0-3.

Sizes used by the code: the memory clears are `0x41` (65) bytes, every receive read is limited to `0x41`, the largest
send is header 2 + payload 64 = 66 bytes (the cap at `0xF8D74`). `0x41` is also what the ring capacities are built
around (F2). The same buffer is therefore used for receive and send; the dumps show a received basic input report
sitting in it. Whether the receive functions run under the same Bluetooth lock as the sender was not checked (the
sender `0x10283C` and the generic wrapper `0x100564` both take the lock whose id is in the `0x4325D0` variable).

### F1.2 What lies around it [code][dump]

| Address | Object | Evidence |
|---|---|---|
| `0x432688` | small variable (`TOC-0x3438`) | one qword, value 1 in the dump |
| `0x432690` | handle table, 7 qwords up to `0x4326C7` | `TOC-0x3410` / `TOC-0x32C0`; the pad-to-connection link of the stage 1b identity note |
| **`0x4326C8`** | **shared packet buffer** | above |
| `0x43270A..0x43270F` | 6 bytes, no reference of any kind | alignment padding (never written by the code that uses the buffer) |
| **`0x432710`** | **Bluetooth HID host client state**, at least `0x30` bytes | `TOC-0x3560`, `-0x3430`, `-0x32A0`, `-0x3058`; 31 load sites in `0xF4xxx-0xFCxxx` and `0x1022xx` |
| `0x432740` | next object (cleared as a unit by `0xFAF08`) | `TOC-0x32B0` |

The nearest symbol after the buffer start is `0x432710`, **72 bytes** away. The buffer is not larger than that.

### F1.3 The object at `0x432710` [code]

- `0xFD040(obj, args, flag)` (reached from `0xF4780`, a function that first copies a 12-byte argument block from the
  caller, so this is a client-registration entry): if the qword at `obj+0` is non-zero it returns `0x8001000A`;
  otherwise it allocates a 0x20-byte heap object, initialises it with `0xFD460` and **stores its pointer at `obj+0`**.
- `0xFAF08(obj)` (called from `0xF67CC` and `0xFAFC4`) is the matching reset: clears the qwords at `obj+0`, `+8`,
  `+0x10`, byte `+0x18`, then bytes up to `+0x26`, and the next object.
- `0xFA9F8(obj, arg)` (from `0xF4CDC`) loads the qword at `obj+0` and calls through that object's vtable.
- Bytes `+0x2A` and `+0x2B` are flags read by the wrapper family at `0x102234` and written by `0xF55F8`.

So the first qword of `0x432710` is the pointer to the registered HID client object: tested for "already
registered", dereferenced for calls. In all 14 local dumps it is zero (nobody registered at that moment). **Writing
the last 7 bytes of a 79-byte packet there would make the pointer non-zero garbage (three zero bytes and the four CRC
bytes of the report, as the top seven bytes of the qword) and leave it there**: the next send clears only 65 bytes, so
the garbage stays until a registration or the next long packet overwrites it. A later registration would then fail
with `0x8001000A`, and any call through the slot would jump through a garbage pointer.

### F1.4 Consequences and candidate directions (not decided)

- A 78-byte DualSense output report cannot be shortened (spec section "DualSense side"), so it needs 79 bytes in the
  buffer. The largest packet that fits without touching `0x432710` is 72 bytes (payload 70).
- The cap patch also lifts the 64-byte limit for the other caller of `0xF8D60`, the generic method at `0x100564`
  (F3), so that path could then overrun the same 7 bytes with a 65-77 byte payload. Today it is rejected.
- **Candidate: move the buffer.** Redirect the two TOC slot qwords (`0x372650` to a shim-owned buffer of at least
  80 bytes, `0x372660` to that address plus 1). All seven load sites then use the new buffer; their own use never
  exceeds 66 bytes (and 65 for receive), so they stay correct. This needs a new qword-data patch kind in the
  installer (hash-checked, reverted on unload) and no call of its own. Not validated here.
- The receive side has the same fixed limit: every receive passes `0x41` as the maximum (`li r7,0x41` at `0xF8484`,
  `0xF9094`, `0xF9134`, `0xF91F4`). For a packet longer than that, `0x1169E4` returns `0x80010028` and leaves the packet
  queued. This matters for the full-report input piece (a `0x31` report is 78 bytes plus header): moving the buffer
  would also need those four sizes raised and the interrupt-in ring capacity (134 bytes) checked.

## F2 - Length limits below `0xF8D60` [code]

Path of a send on channel 0 (the DS3 output path): `0xF8D60` -> tail call `0x116EF0(addr, channel 0, mode 2, buffer,
length+2)` -> mode table (jump table at `0x116FE4`, index 2 is `0x1170F0`) -> `0x1169E4(conn, channel, buffer, length,
0)` -> ring write `0x11E640` -> flush `0x115180` -> HCI/L2CAP packing `0x10B82C` -> HCI ring.

| Layer | Limit |
|---|---|
| `0xF8D74` | payload > 64 refused with `0x80010002`. The compare is `cmplwi cr7,r31,0x40`; the branch skips to the copy when payload <= 64. **This is the only length cap on the path.** |
| `0x116EF0` | channel index must be 0-3, a connection must be found for the address, and the connection flag word must not carry the "closed" bit (mask `0x04000000`, mode 2) - no length test. |
| `0x1169E4` | length must be > 0 (else `0x80010002`); channel must be active. For channel 0/2 (send) the transmit ring needs `capacity - used >= length + 2` (else it returns 0, nothing queued). No other test. Return value = the length queued. |
| Transmit rings (per connection, at connection `+0x12C8 + channel*0x48`) | Observed capacity: **channel 0 and 1 = `0x86` (134), channel 2 and 3 = `0x43` (67)** in every dump with a connected pad (empty rings). A 79-byte packet needs 81 bytes free of 134: fits when the ring is empty. It is drained at once by `0x115180`; the HCI ring (`TOC-0x2CF0`, capacity `0x800` in all dumps, empty) needs `length + 10`. Capacities look built around the 65-byte packet (`0x43` = 65 + 2, `0x86` = two of them); the initialiser was not located, so the values are measured, not derived. |
| `0x115180`, `0x10B82C` | no comparison against a maximum payload or MTU; the record is copied whole (a wrap of the ring is handled by a two-part copy). `0x10B82C` fails with `0x8001002B` if the HCI ring lacks space. |

Not verifiable statically: the negotiated L2CAP MTU of the peer and the dongle's ACL buffer size. A DualSense accepts
78-byte Bluetooth output reports on its interrupt channel (spec, sourced from Linux).

Return value: the send returns the number of bytes queued (the packet length), so a rewritten report makes
`0x10283C` return 79 instead of 50. Section F6 shows that nothing tests that value except for one error code.

## F3 - Callers [code]

Searched over the first 4 MiB of the dump: every `bl`, `b`, `ba` and conditional branch with the function as target,
every qword and word holding the function's descriptor (OPD) address, every TOC slot holding it.

| Function | Direct callers | Indirect references |
|---|---|---|
| `0x10283C` (BT send wrapper, OPD `0x3408E0`) | `bl` at **`0x2A2DE8`** only | none found |
| `0xF8D60` (HID sender, OPD `0x33F680`) | `bl` at `0x1028C4` (inside `0x10283C`) and at **`0x100648`** (inside `0x100564`) | none found |
| `0x100564` (generic send: type 2, report id = first byte of the caller's buffer, rest as payload; takes the same Bluetooth lock) | none | OPD `0x3404C0` is slot 11 of a vtable at `0x37E1A8` (referenced by TOC `-0x30F0` and a heap object at `0x64D400`); its callers through the vtable were not enumerated |
| `0x2A2C30` (DS3 report builder, OPD `0x366728`) | `bl` at `0x2A2E50` (inside `0x2A2DF8`), `bl` at `0x2A4964` (the converter `0x2A4678`, periodic output) and a tail `b` at `0x2A2F2C` (inside `0x2A2EA4`) | none found |
| `0x2A2DF8` | tail `b` at `0x2A37B0` (command dispatcher) | none found |
| `0x2A2EA4` (set rumble: copies the 6-byte request into pad+0x13B) | `b` at `0x2A3AD4`, `bl` at `0x2A4C94` | none found |

`0x2A2C30(pad, led_request, rumble_request, flag)` sends over Bluetooth only at `0x2A2DE8` (taken when pad+0x6C is 2).
USB (pad+0x6C is 1) goes to `0x107C18` (flag 0) or `0x107CD8` (flag non-zero); any other pad+0x6C value sends
nothing. So **every rumble or LED update of a pad object that goes over Bluetooth passes the call at `0x2A2DE8`**.

**Not the only Bluetooth path for a DS3-format output report.** The start-up drivers (code `0xCA2CC` and `0xCA97C`)
build a 48-byte report from an all-zero request with the same packer `0xCC054` and send it through `0x102750` (callers
`0xCA334`, `0xCA9BC`, tail `b` at `0xCA0F0` and `0xCACEC`), report id 1, type 2, handle byte from the driver object.
`0x102750` is the callback variant of `0x10283C` (same lock and connection lookup) but ends in `0xF8BD8`, which sends
SET_REPORT on the control channel (channel 2, ring 67 bytes) and has no 64-byte cap. These sends are untouched by
both patches and carry no rumble or LED state (zero request), so they are not part of the translation; they keep
reaching the DualSense in DS3 format as they do today. The USB twins of those driver sends are `0xCA704` and
`0xCAABC` (`0x107C18`, type `0x201`).

## F4 - The DS3 payload `0xCC054` builds [code] and a real instance [dump]

`0xCC054(dst, capacity, src)`: refuses a null `dst` or capacity <= `0x2F` (`0x80010002`), clears `capacity` bytes of
`dst` (`0x40` at the pad call), then builds `dst` from the 0x20-byte structure `src`. **It returns `0x30` (48) in
every successful case**, so the payload length handed to `0x10283C` is 48 and the full packet is 50 bytes.

Packing, **[code]**:

| `dst` byte (payload index, report id not included) | Content |
|---|---|
| 0 | `src[0]` (copied from pad+0x90, zero) |
| 1-6 | `src[1..6]` verbatim = the six bytes of the rumble request |
| 7, 8 | zero |
| 9 | LED bitmap = `src[7]` AND `0x1E` (only if `src[7]` is non-zero) |
| 0x0A-0x0E | LED block for mask bit `0x10` (from `src[0x1A]`, the halfword at `src[0x1E]` high then low, `src[0x1B]`, `src[0x1C]`) |
| 0x0F-0x13 | LED block for mask bit `0x08` (`src[0x14]`, halfword `0x18`, `0x15`, `0x16`) |
| 0x14-0x18 | LED block for mask bit `0x04` (`src[0x0E]`, halfword `0x12`, `0x0F`, `0x10`) |
| 0x19-0x1D | LED block for mask bit `0x02` (`src[0x08]`, halfword `0x0C`, `0x09`, `0x0A`) |
| 0x1E-0x2F | zero |

A LED block is only copied if its mask bit is set. Mask bit `0x02` is LED 1 ... `0x10` is LED 4, which is the standard
DS3 layout (bitmap byte at report byte 10, LED blocks in reverse order, as in Linux `hid-sony.c`).

How `0x2A2C30` fills `src` **[code]**: `src[0]` = pad+0x90; `src[1..6]` = the six bytes at the rumble request pointer
(third argument); `src[7]` = (byte 3 of the LED request, the second argument) shifted left by one; then four
blocks of six bytes at `src[8+6i]` from the LED request: `src[8+6i]` = request byte `7+4i`, `src[9+6i]` = request byte
`0x27+4i`, `src[0xA+6i]` = request byte `0x17+4i`, halfword at `src[0xC+6i]` = request bytes `0x36+4i`, `0x37+4i` (high, low).
The LED request is the 0x44-byte block at pad+0xF7, the rumble request the 6 bytes at pad+0x13B.

So, by the standard DS3 meaning of those bytes (**[assumed]**, from Linux `hid-sony.c`, consistent with the dump
below): payload byte 1 = right motor duration, 2 = right (small) motor on/off, 3 = left motor duration, 4 = left
(large) motor strength 0-255, 5-6 = whatever the request carries (zero in practice), 9 = LED bitmap.
In report coordinates (with the id) add 1: rumble bytes 2-5, LED bitmap byte 10.

Who writes the rumble request **[code]**: (a) the converter block at `0x2A4938..0x2A4964` (reached from its periodic
path at `0x2A48F0`) stores the constant `0x96` at pad+0x13B and +0x13D, the byte pad+0x220 (small motor) at +0x13C
and the byte pad+0x221 (large motor) at +0x13E, then builds the report; the conditions that lead there (a non-zero
motor byte, an elapsed-time test with the constant `0x3E7F`, the flag pad+0x229) and the order of the clearing of
pad+0x220/0x221 were not traced to the end; (b) the set-rumble method `0x2A2EA4` copies the six bytes of its caller's
request to pad+0x13B and builds at once (the bytes are the pad manager's, not constants).
**Durations are therefore finite (`0x96`) at least in the periodic path**; whether the pad manager's direct requests
carry `0xFF` or `0x96` is only visible in a capture (the observe stage). A DualSense keeps vibrating until told to
stop, so a stateless translation must not depend on the DS3 duration expiring. Open until captured.

**[dump] A real DS3's idle report** (pad `0x661000` in `ds3route/navpid_connected.bin`, `type01_connected.bin`,
`twopads/ds45pad_lv2.bin`; identical): payload byte 1 = `0x96`, byte 3 = `0x96`, bytes 2 and 4 = 0 (motors idle),
byte 9 = `0x02` (LED 1 on), bytes 10-24 zero (LEDs 4, 3, 2 blocks), bytes 25-29 = LED 1 block (`0xFF`, `0x00`, `0x01`,
`0x00`, `0x01`), the rest zero; the LED request has byte 3 = 1. The rumble request holds `0x96 0x00 0x96 0x00 0x00
0x00`. The DualSense pad object in the same dump never built a report (buffer and requests zero) but has
pad+0x220/0x221 non-zero. This confirms the layout above for the idle state; a capture with the motors on is still
needed for the motor bytes.

## F5 - Hash rows [code]

Method: `ds45_fnv1a64` semantics (offset basis `0xCBF29CE484222325`, prime `0x100000001B3`, bytes in memory order) over
the 8-aligned qword, on `analysis/lv2_code_492.bin` and on `ds45pad_lv2.bin` (identical result). Cross-checked three
ways: the repo's Python port `tools/lv2scan.py` (`fnv1a64`), an independent loop that works from the qword value
(most significant byte first), and a C build (clang) of a verbatim copy of `ds45_fnv1a64` from `src/lv2.c`; the C copy
also reproduces the test vector for `"a"` (`0xAF63DC4C8601EC8C`). `src/lv2.c` itself needs the PS3 SDK headers and was
not compiled.

| Row | Address | Aligned qword | Instruction | FNV-1a 64 of the qword |
|---|---|---|---|---|
| output call | `0x2A2DE8` | `0x2A2DE8` (the call is the high half, the next instruction `lbz r0,0x6C(r30)` the low half) | `bl 0x10283C` (displacement -0x1A55AC) | **`0x2A29E6BB7BA10D5E`** |
| send cap, original | `0xF8D74` | `0xF8D70` (the compare is the low half, the high half is `std r26,0x80(r1)`) | `cmplwi cr7,r31,0x40`, word **`0x2B9F0040`** | **`0xBF27C253F5228E13`** |
| send cap, patched | `0xF8D74` | `0xF8D70` | `cmplwi cr7,r31,0x4D`, word **`0x2B9F004D`** | `0xBF27CD53F522A0C4` |

The patched hash of `0x2A2DE8` depends on the branch target in the blob, so it is derived at install time. The cap
word is the original with its low 16 bits (the immediate) changed from `0x40` to `0x4D`; nothing else differs.

## F6 - Stub contract for a `bl` entry at `0x2A2DE8` [code]

State at the moment the `bl` executes (inside `0x2A2C30`, BT branch):

| Register | Value |
|---|---|
| `r2` | kernel TOC `0x80000000003759C0` (the function never changes it; the call is a direct `bl` to code, not through a descriptor, so the kernel `r2` is what `0x10283C` expects) |
| `r3` | byte `pad+0x20` (zero-extended; the connection handle byte) |
| `r4` | 2 (report type: output) |
| `r5` | 1 (report id) |
| `r6` | `pad+0x22B` (payload pointer; **pad = r6 - 0x22B**) |
| `r7` | 48 (`0x30`, the packer's return value, zero-extended to 16 bits) |
| `r30` | pad object |
| `r29` | `pad+0x22B` |
| `r28` | the function's 4th argument (0 or 1; unused on the Bluetooth path) |
| `r31` | the packer's return value (48); overwritten after the call |
| `r1` | frame of `0x2A2C30` (0xB0 bytes; the saved link register of its own caller is at `0xC0(r1)`, locals at `0x70..0x8F`, saved `r28..r31` at `0x90..0xA8`) |
| `r14..r27` | belong to the callers of `0x2A2C30`; untouched by it, so the stub must preserve them |
| `lr` | set by the `bl` to `0x2A2DEC` (the instruction after the call) |

After the call (`0x2A2DEC`): it reloads `lbz r0, 0x6C(r30)` (so `r30` must survive), copies `r3` to `r31`, and branches
to `0x2A2D5C`, which for a Bluetooth pad (type 2, not 1) goes straight to the epilogue and returns `r31` sign-extended
as the function's result. `r0`, `r8..r12` and `cr7` are dead at the call. The result is tested only by the callers:
`0x2A2E54` and `0x2A4968` compare it with the error `0x8001000A` ("nothing to send", which `0x2A2C30` itself returns
when pad+0x22A is zero); anything else is passed up or ignored. A tail call to `0x10283C` that returns the original
result (50 today, 79 after a rewrite) is therefore safe.

Consequences for the entry, as the spec already intends: it is entered by `bl`, so its own return address is in `lr`
and the tail call to `0x10283C` must restore `lr` and the kernel `r2` (adopt the blob TOC, save the kernel `r2` in a
frame, restore both, pop the frame, branch). The branch distance from `0x2A2DE8` to the blob entry must be within
+-32 MiB. The stub may use stack below `r1`; it must not touch the frame above it.

## F7 - Console observe run (2026-11-03, plan task 6) [console]

Build: observe mode (`SHIM_OUTPUT_HOOK = 1`, `SHIM_OUTPUT_MODE = 0`), data block v9. Pads: a DS3 over Bluetooth only
(player 1) and one DualSense (player 2) in a split-screen game; also earlier runs with two DualSense.

**Safety (passed).** The `bl` entry stub and the moved packet buffer ran live: no freeze or disconnect across several
reboots and reconnects, `padmiss` 0, pad input still translated (`xl` rising) through the buffer at `pktbuf`, the DS3
fully usable. Read back after install: the call at `0x2A2DE8` is a `bl` into the blob (stub at blob offset `0x90`), the
slots `0x372650` and `0x372660` hold `pktbuf` and `pktbuf + 1`, the cap word is unchanged in observe mode.

**Report stream.** The kernel sends a DS3-format output report for every pad object continuously, even idle: about 55
per second across three pad objects (17,370 calls in roughly 5 minutes). Every payload is 48 bytes. A sustained rumble
therefore always ends with a zero-motor report within milliseconds, and the once-a-second sample of the first runs
(latest payload, random pad) simply missed the rumble; the rumble-aware capture (task 6b) did not.

**Captured payloads** (48 bytes, report id not included; offsets confirm F4):
- idle, player 1 (DS3): `00 96 00 96 00 00 00 00 00 02 00 ...`, LED block 1 at bytes 25-29 (`FF 00 01 00 01`);
- idle, player 2 (DualSense): bitmap `04`, block at bytes 20-24; player 3: bitmap `08`, block at bytes 15-19;
- **rumble on** (game rumble on the DS3 and on the DualSense): `00 96 01 96 00 00 00 00 00 <bitmap> ...`, i.e. byte 1 =
  right duration `0x96`, byte 2 = small motor `01`, byte 3 = left duration `0x96`, byte 4 = large motor (0 in the last
  captured on-report; the strength is a plain 0..255 byte), byte 9 = the LED bitmap;
- **rumble off**: the first report from the same pad afterwards is the idle report with bytes 2 and 4 zero.
Counters at the end of the run: 192 motor-on reports, 24 captured stops, 15,413 output calls.

**Decisions this settles.**
- The rumble is stateless on the kernel side: every report carries the full current motor state, durations are always
  the constant `0x96`, and the kernel sends a zero-motor report when a rumble ends. The translation can ignore the
  durations and copy the motor bytes (small: nonzero means on; large: the strength).
- The spec's open question about a stop timer is closed: none is needed.
- Not yet seen: a captured large-motor value above 0 (the games used the small motor in the last captured reports); the
  translation copies the byte as planned.

## F8 - Console probe run (2026-11-03, plan task 9) [console]

Build: probe mode (`SHIM_OUTPUT_MODE = 1`: every DualSense pad gets a fixed report, blue light bar and player LED 1, no
rumble), data block v9, cap word raised to `0x4D`. Read back after install: cap qword `0xF8D70` low half `2B9F004D`, call
site `bl` into the blob, slots at `pktbuf` / `pktbuf + 1`. Pads: a DS3 over Bluetooth and one DualSense.

**Output works.** The DualSense accepted the rewritten 78-byte report **while in its basic input mode** and the light bar
turned blue (user). No hang, no disconnect; the 79-byte packet went through the moved buffer and the raised cap. 23 of 47
output calls were rewritten (the DualSense's), the DS3's 24 passed through untouched, the DS3 stayed fully usable.

**Regression: the DualSense's input stopped.** `xl` (translated reads) stayed at 1 from the first rewritten report on,
while `inds` (reads seen on the DualSense's transport) crawled to 142 in about 100 s instead of the thousands seen before.
The user confirms no input on the DualSense; the DS3 is unaffected.

**Likely cause (not proven).** A DualSense on Bluetooth sends the short basic report 0x01 until the host sends it an
output report 0x31, and then switches to the full 78-byte 0x31 input report (plus one header byte = 79 bytes). The HID
receive functions of this kernel read at most `0x41` (65) bytes (F1.1) and for a longer packet return `0x80010028` and
leave it queued (F1.4), so the input stalls while the poll keeps retrying. If that is right, the output piece cannot ship
alone: it needs the receive side to take 79-byte packets, which is the stage 2 full-input piece (the translator for the
0x31 report already exists in shim/ds3_report.c).

**Not yet checked.** The raw reads the input hook sees after the switch (no instrumentation records the read result or
the first bytes), and the capacity of the interrupt-in ring.

## F9 - The receive path of a DualSense input report (read-only, plan task 11)

Static analysis of `ds45pad_lv2.bin` (code identical to the pristine image at every site named here, checked for the
four immediates) plus the connection tables of the connected-pad dumps. Nothing was sent to a console.

### GATE: FAILED - raising the read limit alone would overflow the pad driver's stack buffer

**The copy into the caller's buffer is not bounded by the caller's length.** The HID receive function `0xF8FE0` (the
`0xF9060` block is its interrupt-in body) only tests that the length cell is non-zero, then copies `read - 1` bytes to
`dst` with the kernel `memcpy` (call at `0xF91A8`). The only bound is the `0x41` read limit. The pad driver's `dst` is a
**stack local of 64 bytes** (`len_in` = `0x40`), directly followed by saved registers (F9.4). With the limit raised to
`0x50`, a 79-byte `0x31` record is copied as 78 bytes: **14 bytes past the 64-byte local, into the saved
registers of the pad driver's frame** (in one caller 6 bytes past a 72-byte slot into saved `r27`; in the other the
saved `r28` and 6 bytes of `r29`). That corrupts the caller's registers on return: a crash or hang class of fault.

The ring itself is fine (it holds the 81-byte record, F9.3), and no other layer rejects a 79-byte packet (F9.2). The
**safe shape** is therefore not "raise four immediates" but "raise the interrupt-in immediates **and** make the input
hook call the original with a shim-owned buffer of at least 80 bytes, then copy at most `len_in` translated bytes to
`dst`" (F9.6, F9.7). That changes the design (Amendment 2 assumed the hook only needed its guard relaxed), so the
decision belongs to the user. Not decided here.

| Question | Answer |
|---|---|
| Is the copy into `dst` bounded by `len_in`? | **No** (F9.1). Bounded only by the read limit `0x41` today. |
| Does the pad driver's `dst` hold more than 64 bytes? | **No**: 64 bytes (72 in one caller before a saved register). Needs 55 only for the translator, 78 for the raw report (F9.4). |
| Does the interrupt-in ring hold a 79-byte packet? | Yes: capacity 134, the record needs 81 (F9.3). |
| What does an over-long record do today? | `0x1169E4` returns `0x80010028` **after having consumed the 2-byte length prefix** and leaves the body in the ring: the ring is then out of frame until the writer overflows it (F9.2). Worse than "left queued". |
| Is raising the limit safe for the DS3? | Yes for the DS3 and the basic-mode DualSense (records of 50 and 11 bytes, below 65); it only changes the outcome for records of 66 to 80 bytes (F9.5). |

### F9.1 The path of one received interrupt-in packet [code]

1. **ACL data and L2CAP.** The HCI ACL data handler (frame start `0x10BC4C`; the reassembly and dispatch code read is
   `0x10BDC4..0x10BE90`, the rest of the handler was not traced) reassembles the L2CAP frame in the
   connection (area at connection `+0xEA6`, running length at `+0xEA4`, expected length at `+0xEA0`). A frame whose
   running length would exceed `0x400` (1024) is dropped (compare at `0x10BDD4`), so 79 bytes pass. It looks the
   channel up by CID (`0x1127B8`) and calls the channel's callback through the descriptor stored at channel `+0x1F8`,
   with the connection, the channel, a pointer to the L2CAP payload and its length (payload length = frame length - 8).
2. **HID data callback `0x114DC0`** (descriptor `0x3426C8`; it is referenced by that descriptor only, so the L2CAP layer
   is its only caller). Channel PSM `0x13` (interrupt) writes one record into the connection's **channel 1 ring**
   (ring struct at connection `+0x1310`, the `+0x1308` base at `0x114F04`); PSM `0x11` (control) goes to channel 3
   (`+0x13A0`). A record is `[16-bit big-endian length][payload]`, where the payload includes the HID header byte
   (`0xA1` for a DATA input transaction): 2 + length bytes in the ring. No length test other than free space: if
   `capacity - used < length + 2` the ring is **reset** (indices and used set to zero, a drop counter at ring `+0x10`
   incremented, `0x114E7C..0x114EAC`) and the write is retried; if it still does not fit the record is dropped. After
   the write it signals the HID layer through `0x115524(connection, 1)` for the connected states.
3. **Reader.** The pad driver input caller `0x2A4E00` loads the pad's transport (`pad+0x4F0`) and calls vtable slot `+0x20`
   (slot `0x37CE38`, descriptor `0x33A010`) with `r3` = transport, `r4` = `dst`, `r5` = `len_in` (passed through from its
   own caller), `r6` = 0. The descriptor is the method at `0xC9D60`: it stores `len_in` in a local halfword and calls
   `0x101E20(handle byte at transport+8, dst, &len, flag XOR 1)`; on success the method returns the updated halfword
   (the byte count), on failure the negative error code. `0x101E20` takes the
   Bluetooth lock (variable `0x4325D0`), finds the HID client for the handle (`0x1010B8`) and calls `0xF8FE0(client, dst,
   &len, flag)`.
4. **`0xF8FE0`** returns `0x80010002` for a null `dst`; its interrupt-in body (`0xF9060`) returns the same if `*len` is 0,
   clears 65 bytes of the shared buffer, calls `0x116EF0(addr, channel 1, mode 2, buffer, 0x41)` (mode 2 on the
   receive channels is the read: `0x1170F0` rejects a closed connection (bit `0x04000000`) and calls
   `0x1169E4(conn, 1, buffer, 0x41, 0)`). With the flag non-zero (the pad driver passes 0, which `0xC9D60` turns into 1
   by the XOR) it reads **a second time** into the same buffer; a second record wins, otherwise the first stays. Then it copies
   `read - 1` bytes from `buffer+1` (slot `TOC-0x3360`) to `dst`, stores that count in `*len` and returns 0.
5. **Return values.** `0xC9D60` returns `read - 1` (the packet length without the `0xA1` header): **49 for a DS3 record
   (50 bytes in the ring), 10 for a basic-mode DualSense (11 bytes), 78 for a `0x31` record (79 bytes)**; `dst[0]` is
   the report id. Today a `0x31` record gives the hook the negative `0x80010028` (the guard passes it through
   unchanged) and, until the ring is rewritten, a 0 (F9.2). After a limit raise the hook would see 78, which the
   existing `rc > 0x41` guard rejects (it would hand the raw report to the pad driver).

**[dump]** The `navpid_connected` and `type01_connected` dumps show the ring contents of a DualSense in basic mode:
13 bytes per record (length `0x000B`, then `A1 01` and nine bytes), 9 records queued in the first.

### F9.2 What `0x1169E4` does with an over-long record [code]

Receive branch (channels 1 and 3): if the ring holds at most 1 byte it returns 0 (nothing). Otherwise it reads the
2-byte length prefix with `0x11E440` (a **consuming** read: read index and used count are updated) and compares the
remaining used bytes with the record length; if the record is incomplete it **gives the 2 bytes back** with `0x11DC5C`
and returns 0. If complete, it compares the caller's maximum with the record length (compare at `0x116D2C`): if the
maximum is **smaller**, it returns **`0x80010028`** (`lis`/`ori` at `0x116D34`) and **does not give the prefix back**.
Otherwise it copies the record and returns its length.

Effects of the over-long case (a `0x31` record against the limit `0x41`):
- The record body stays in the ring, **two bytes later in frame**. The next read takes the first two body bytes
  (`A1 31`, = 41265) as the length, finds fewer bytes than that, restores the 2 bytes and returns 0, and keeps doing
  so (the hook sees `rc` 0 and a length of 0, and the translator refuses it).
- The ring recovers only when the writer overflows it: a 134-byte ring holding a 79-byte stale body cannot take the
  next 81-byte record, so `0x114DC0` resets the ring and writes the new record in frame, and the next read fails with
  `0x80010028` again. So at most one error per incoming record, and **no report is ever delivered**. This matches F8
  (`xl` frozen at 1 from the first rewritten report on).
- No packet is lost on the wire, and nothing else is changed (no event, no disconnect). The rate of about 1-2 hook
  entries per second seen on the console is not explained by this code; the entries show the pad driver keeps
  polling the transport. **[assumed]** the poll is driven by the HID layer's notification and by a timer, not
  traced.

No other reason to reject a 79-byte packet exists on this path: `0x116EF0` and `0x1170F0` test the channel index, the
connection and the "closed" flag only; the HID layer tests `dst` non-null and `*len` non-zero (F9.1).

### F9.3 Capacity of the interrupt-in ring [code][dump]

- **[code]** The HID client open function (`0xF9284`) opens the four channels with `0x116EF0` mode 0 and a buffer
  inside the client object: channel 0 `0x86` bytes at client `+0x18`, channel 1 `0x86` at `+0x9E` (call at `0xF93F0`,
  immediate `li r7,0x86` at `0xF939C`), channels 2 and 3 `0x43` each, **both on the same buffer** at `+0x124`
  (`0xF9424` and `0xF9494`). The capacity is the size handed to the connection open (`0x1170D8` stores it at ring
  `+0xC`). The ring buffers lie back to back in the client object, so **raising a capacity immediate would run
  into the next buffer**; it is not needed.
- **[dump]** Measured in `navpid_connected`, `type01_connected`, `twopads`, `twodualsense_snap`: channel 1 capacity `0x86`
  (134), channel 3 `0x43` (67), for the DualSense's connection and the DS3's alike.
- A `0x31` record needs `2 + 79 = 81` bytes of 134: **fits** when the ring is empty or holds at most 53 bytes of other
  records. A second record arriving before the first is read overflows the ring and resets it (the unread one is dropped,
  the newest is kept), so a slow reader sees the newest report only. The DS3's 52-byte records fit twice (104).

### F9.4 `len_in` and `dst` as the pad driver passes them [code]

`0x2A4E00` has two callers, both stack-local readers of the pad object:
- `0x2A50FC` (frame `0xE0`, call at `0x2A51E8`): `dst` = `r1+0x70`, `len_in` = `0x40` (`li r5,0x40` at `0x2A51E4`). The
  first saved register (`r27`) is at `r1+0xB8`: **72 bytes** of local space.
- `0x2A521C` (frame `0xD0`, call at `0x2A528C`): `dst` = `r1+0x70`, `len_in` = `0x40` (`li r5,0x40` at `0x2A5280`). The
  first saved register (`r28`) is at `r1+0xB0`: **exactly 64 bytes**.
Both pass the returned count to the DS3 converter `0x2A4678` (with `dst` and that count). So `dst` holds 64 bytes: the
translator's 55-byte minimum fits, the 78-byte raw report does not. The DS3's own 49-byte reports use 49 of the 64
and are unaffected. The translator output is 49 bytes (`DS3_REPORT_LEN`), written in place of the input.
After the read `0x2A4E00` returns the count; a negative value ends the poll early. A non-zero count with `dst[0]`
= 1 and `dst[1]` != `0xFF` goes on to `0x2A26A8` with that count.

### F9.5 Users of the receive functions and the effect of a limit raise [code]

| Function | Channel | Callers | Effect of `0x41` -> `0x50` |
|---|---|---|---|
| `0xF8FE0` (interrupt-in body `0xF9060`: reads at `0xF9094`, `0xF9134`) | 1 | `0x101E20` (reached only from `0xC9D60`, i.e. the pad transport), `0x100130` and `0x1007B8` (inferred: a generic asynchronous read with a caller-supplied buffer and length, vtable client API; their callers were not enumerated) | DS3 (49-byte result), basic DualSense (10) and every record up to 65 bytes: **unchanged**. Records of 66-80 bytes: delivered instead of failing. The two generic callers get the same unbounded copy; their buffers are sized by their callers and were not checked. |
| `0xF91C0` (drain, read at `0xF91F4`) | 1 | `0x101DF0`, from the wrapper `0x101D98` (descriptor `0x340790`) | Reads up to two records and discards them into the shared buffer (no `dst`). Safe to raise; unraised, a pending `0x31` record would be swallowed with the prefix and desync the ring. |
| `0xF840C` (control in, read at `0xF8484`) | 3 | callback, not enumerated | Leave as is: the channel-3 ring is 67 bytes and cannot hold a record above 65 anyway. |

The 65-byte clears in these functions (`li r5,0x41` before the memset) only pre-clear the shared buffer; the copy that
follows is exact (`read - 1` bytes), so no stale bytes are delivered and they need no change. The moved shared buffer
(96 bytes, Amendment 1) holds an 80-byte read plus the `buffer+1` view.

### F9.6 Recommended patch set [code]

Limit change only for the interrupt-in channel. `li r7,0x41` is the word `0x38E00041`; the new word is
`li r7,0x50` = `0x38E00050` (80 >= the 79-byte record; 0x4F would be the minimum). All three instructions are the
**low half** of their 8-aligned qword. FNV-1a 64 over the qword (offset basis `0xCBF29CE484222325`, prime
`0x100000001B3`, bytes in memory order), computed three ways (byte-order loop on the dump and on the pristine image,
which agree; a loop that works from the qword value most significant byte first; PowerShell `BigInteger`):

| Site | Function / role | Aligned qword | Original hash | Patched hash (word `0x38E00050`) |
|---|---|---|---|---|
| `0xF9094` | `0xF8FE0` first read, channel 1 | `0xF9090` | **`0x069BC077D72F4325`** | `0x069BCF77D72F5CA2` |
| `0xF9134` | `0xF8FE0` second read (flag set), channel 1 | `0xF9130` | **`0x57006EC0FDB88DBC`** | `0x57005FC0FDB8743F` |
| `0xF91F4` | `0xF91C0` drain, channel 1 | `0xF91F0` | **`0x995A6CDC70F64A38`** | `0x995A7DDC70F6671B` |
| `0xF8484` | `0xF840C` control in, channel 3 | `0xF8480` | `0x069BC077D72F4325` | **not recommended** (would be `0x069BCF77D72F5CA2`) |

The qwords at `0xF9090` and `0xF8480` have the same bytes (the high half is the load of a word from `8(r9)`), so they
share both hashes; the installer keys the check by address. Required: the first two rows (without them the packet is
refused or the second read desyncs it). Recommended: the drain row. Nothing else changes: ring capacity, the shared
buffer size and the 65-byte clears stay.

**Safer or smaller options.**
- Channel 3 stays untouched (above), so the change is already limited to the interrupt-in channel.
- **A bare limit raise is not acceptable** (GATE). It must be paired with the bounce buffer in the hook (F9.7). The
  bounce also covers the DS3 and every other pad read through the transport, with no change in what they see.
- Truncating at the kernel instead (deliver the first 64 bytes of the record and drop the rest) would need new code in
  `0x1169E4` (a clamp plus a discard of the remainder), not a word patch, and would touch every user of the ring; the
  translator needs only 55 bytes, so it would work, but it is a larger and riskier change than the hook bounce.
- **[assumed]** The generic read callers (`0x100130`, `0x1007B8`) are not used on a pad's connection; if one were, a
  `0x31` record could overrun its buffer. They are unreachable for the pad driver's own read, which is the only reader
  the shim wraps.

### F9.7 What the hook and the filter must change [code]

`shim/entry.c` `shim_input`:
1. Call the original with a **bounce buffer of at least 80 bytes** on the hook's own stack (a static buffer in the blob
   would be shared; the original runs under the Bluetooth lock but the hook's tail does not), with the caller's
   `len_in` unchanged (the HID layer only tests it for non-zero). Always, whether or not the shim is enabled, as
   long as the limit patch is installed: the bounce is what keeps `dst` safe.
2. Relax `rc > 0x41` to the real maximum (`rc > 0x4F` = 79, the most the raised limit can return), keeping `rc < 0` as is.
3. For a DualSense run the filter on the bounce buffer (length `rc`; it translates 0x01 or 0x31 in place and sets the
   length to 49), then **copy at most `len_in` bytes to `dst`**: 49 bytes for a translated report. For any other
   result copy `min(rc, len_in)` bytes, which is exactly what the original used to deliver (a DS3's 49 bytes). An
   untranslatable result longer than `len_in` (a `0x31` report the translator refuses) should return 0 (no data) rather
   than a clipped raw report.
4. The existing check `len_in < DS3_REPORT_LEN` stays meaningful: it now guards the 49 translated bytes written to `dst`
   (the pad driver passes 64).

`shim/shim.c` `shim_input_filter`: **no change needed.** For a `0x31` report it is called with `buf[0]` = `0x31` and
`*len` = 78 (the bounce copy), above `DS_BT_BODY_MIN` (55); `ds3_report_from_dualsense` reads through the status byte
only and writes 49 bytes, then the filter sets `*len` = 49. Its behaviour on the basic 0x01 report (length 10) and on
refusals is unchanged.

### F9.8 What makes the pad switch to the `0x31` input report [code][assumed]

- **[code]** The kernel never sends a Bluetooth HID GET_REPORT. A scan of `0xF4000..0x105000` for the GET transaction
  headers (`ori` with `0x40`/`0x43`) finds only the sample-rate code (`0xF7BE0`, `0xF7C0C`, a flag bit, not a header);
  together with the earlier full scan (`2026-10-30-ds3-route-facts.md` F3) the one sender of control transactions,
  `0xF8BD8`, ORs `0x50` (SET_REPORT) into the header. So the feature report `0x05` that Linux uses to switch a
  DualSense is never requested by this kernel; its host traffic is SET_REPORTs (the `0xF4` enable, the zero DS3
  output report) and interrupt-channel output reports.
- **[dump][console]** In the F8 probe run the input stalled from the first rewritten output report `0x31`, with nothing
  else new sent: the output report is the switch trigger in practice. **[assumed]** the DualSense switches to `0x31` on
  receiving an output report `0x31` while in basic mode; the pad side is not determinable from the kernel image.
- Consequence: once any rewritten output report is sent, the pad stays in `0x31` mode until it reconnects; there is
  no kernel-side way back.

### F9.9 Open

- The generic read callers `0x100130` and `0x1007B8`: answered in F10.2 (heap staging buffer, 58 bytes of room).
- Whether the `0xF840C` callback copies unbounded as well (irrelevant while its limit stays `0x41`).
- The low hook entry rate after the stall (F9.2): explained in F10.6 (the counter skips error reads).
- The negotiated L2CAP MTU and the dongle's ACL size for 79-byte frames: not statically verifiable (as F2); the
  console probe proves the pad's output path, the input side is unproven until a console run.

## F10 - Other receive callers and the transport read contract (read-only, plan task 12)

Static analysis of `ds45pad_lv2.bin` (code identical to the pristine image at every site named here) plus heap objects
read from the connected-pad dumps. Nothing was sent to a console. The decision checked: the interrupt-in (channel 1)
read limit goes from `0x41` to `0x50` (sites `0xF9094`, `0xF9134`, `0xF91F4`) and the input hook calls the original
transport read with a shim-owned 96-byte bounce buffer and `len_in` = `0x50`, for DualSense pads only.

### GATE: PASSED, resting on two assumptions that the kernel image cannot settle

No caller other than the pad driver family can be handed a 66-80 byte record by a device that is in use here, **as
long as** assumption A1 and A2 hold. Both are statements about the devices, not about the code, so they are marked
**[assumed]** and must be said to the user:

- **A1 [assumed]** A PlayStation Move motion controller's Bluetooth input report is 49 bytes (a 50-byte record), as
  the DS3's. A Move is the one in-use device that reaches a caller with a destination under 79 bytes: the generic
  HID client path (F10.2), whose destination is a heap staging buffer with 58 bytes of room (`len_in` `0x3A`). 50 is
  below the old limit, so for it nothing changes. If a Move ever sent a record of 66-80 bytes the raised limit would
  overrun that buffer by up to 20 bytes (the old limit already allowed an overrun for records of 60-65 bytes).
- **A2 [assumed]** A DualSense is not on the generic path while in the `0x31` mode. It reaches the generic path
  whenever its stored device type is not `0x01` (a stock-paired DualSense has type `0x08`; the type `0x01` that the
  DS3 route needs is a manual flip, F10.2); such a DualSense is no pad object, the shim never rewrites an output
  report for it, and it stays in the basic mode (11-byte records), which fits. The kernel itself never sends a
  DualSense a report that switches it (F9.8), and the pad mode is reset by a reconnect.

**Residual risks, not gates** (devices not in use here): a DualShock 4 or any other long-report device on the generic
path (the kernel never sends GET_REPORT, so a DS4 should stay on its short report, F9.8, **[assumed]**); keyboards and
mice (reports of a few bytes); the PS3 Blu-ray remote (12-byte class, F10.1). The remote's driver reads through the
same transport method as the pad driver into a 64-byte stack local, so a long record from it would overflow like the
pad driver's. **Hardening recommended, not required:** let the hook bounce *every* transport read (not only a
DualSense's) and clamp the copy to `len_in`. The transport read method `0xC9D60` is the single choke point of both
driver classes, so that closes the stack-local risk for all of them at the cost of one 96-byte copy per read; the
generic path has no such choke point and keeps the A1/A2 dependency.

Other results: returning 0 is harmless (F10.4); the original is safe to call with a different `dst` and
`len_in` = `0x50` (F10.5); the shared buffer must be the moved 96-byte one before the limit is raised (F10.7).

| Question | Answer |
|---|---|
| Who reaches the channel-1 receive function `0xF8FE0`? | Three direct callers: `0x101E9C` (pad transport, the only one that matters for pads), `0x1007B8` and `0x100130` (the generic HID client). `0xF9060` is not a function of its own, only the second half of `0xF8FE0`. |
| Which destinations are small? | Pad driver 64-72 byte stack locals; second driver class 64-72 byte stack locals; generic client a heap staging buffer, 58 bytes of room. All smaller than 79. |
| Does returning 0 or an error cost anything? | No state change, counter or disconnect (F10.4). |
| Safe to call the original with the bounce arguments? | Yes (F10.5). |
| Why did the poll crawl in the probe run? | Probably it did not: the shim's counter skips error reads, which are now the majority (F10.6). |
| Side effects of the three word edits? | None found on the DS3 or the basic-mode DualSense; the limit is used in exactly one compare (F10.7). |

### F10.1 Every caller of `0xF8FE0`, `0x101E20`, `0xF91C0` [code]

Searched over the first 4 MiB: every `b`, `bl` and conditional branch with the function as target, every qword
holding the function's descriptor (OPD) or the OPD's address, the TOC slots.

| Function | Callers | Notes |
|---|---|---|
| `0xF8FE0` (OPD `0x33F6B0`, nothing references the OPD) | `bl` at `0x101E9C` (inside `0x101E20`), `bl` at `0x100130` (inside `0x1000E0`), `bl` at `0x1007B8` (inside `0x10069C`) | Always channel 1: it does not take a channel; `0xF9060` is its `bne` target at `0xF9020`, the only reference. |
| `0x101E20` (OPD `0x3407A8`, no reference) | `bl` at `0xC9D7C`, inside the transport read method `0xC9D60` | Reached only through vtable `0x37CE18` slot `+0x20` (OPD `0x33A010`, referenced once, at `0x37CE38`). |
| `0xF91C0` (drain, OPD `0x33F6C8`, no reference) | `bl` at `0x101DF0`, inside the wrapper `0x101D98` | The wrapper is entered by the `b` at `0xC9D5C` from `0xC9D58`, the transport method at slot `+0x28` (OPD `0x339FF8`, referenced once, at `0x37CE40`). |

**Who calls transport slot `+0x20` (read) and `+0x28` (drain).** The transport object is built by `0xC9EF4`, called
at two places only: `0x2A4078` (the pad driver class, vtable `0x38CDC8`, the transport kept at pad `+0x4F0`) and
`0x2AB184` (a second driver class, vtable `0x38CF48`, constructor `0x2AB120`, the transport at object `+0xE8`). The
only read calls are `0x2A4E5C` (pad class, inside `0x2A4E00`) and `0x2AB448` (second class, inside `0x2AB250`).
The other loads of the transport (`+0x10`, `+0x18`, `+0x28` at `0x2A256C`, `0x2A25AC`, `0x2A3E98`, `0x2A41AC`,
`0x2A4350` ...) use other slots. [code]

| Reader | Called from | `dst` | `len_in` | Device |
|---|---|---|---|---|
| `0x2A4E00` (pad class) | `0x2A50FC` (call `0x2A51E8`, frame `0xE0`) and `0x2A521C` (call `0x2A528C`, frame `0xD0`) | stack local `r1+0x70`: 72 bytes before a saved register in the first, exactly 64 in the second (F9.4) | `0x40` | DS3, Move navigation (PID `0x042F`, no longer if the PID patch removes it) and the DualSense of type `0x01`: every device the factory `0x2A128C` builds as the `0x520`-byte pad object |
| `0x2AB250` (second class) | `0x2AB60C`, `0x2AB6E8` (frame `0xE0`, `dst` `r1+0x70`, saved `r27` at `+0xB8` = 72 bytes) and `0x2AB7C4` (frame `0xD0`, saved `r28` at `+0xB0` = 64 bytes) | stack local `r1+0x70` | `0x40`, call at `0x2AB438` with the flag argument 1 (so the original reads once, not twice) | The factory's type-4 object (`0xF0` bytes, `0x2AB120`, dispatch at `0x2A13CC`), which copies 12-byte reports to its cache at `+0x22`. **[assumed]** the PS3 Blu-ray remote (the HID layer registers a PID `0x0306` device for type 2, F10.2). Not in use here. |

Both manager entries that reach the pad read methods (`0x29EDBC`, calls at `0x29EEB0` and `0x29EF2C`; `0x29F2D0`,
call at `0x29F3EC`) hold the pad manager lock (`mgr+0x40`) across the call, so two pads are never inside the read
method at once. [code] (Other call sites of vtable slots `0x40`/`0x48` in `0x29xxxx` take small integers, not a
buffer; they are other classes.)

**The drain** `0xF91C0` (the read at `0xF91F4`) is reached from the pad class's and the second class's "clear input"
method (vtable `+0x60`: `0x2A3E60` and `0x2AB1E4`, which guard on the object state being 1 and tail into transport
slot `+0x28`). Their callers are the pad manager's loop over its 16 pad entries (call at `0x29FD40`) and the pad
command dispatcher (call at `0x2A37B8`); the return value is ignored by the loop. The drain reads two records into the
shared packet buffer and discards them; nothing consumes them. With the limit raised a pending 79-byte record is
simply discarded whole (unraised it is swallowed with its length prefix, F9.2). It writes no `dst`. [code]

### F10.2 The generic HID client path: `0x100130` and `0x1007B8` [code][dump]

What they are. Every Bluetooth connection gets a HID client object (vtable `0x37DFA0`, built by `0xF9284`, which opens
the four channels). The paired device's stored type word (`0x115F70` reads it from the remembered-device table at
`0x434910`: 32-byte entries, bd address first, type word at `+8`; it is the link-key type byte of the DS3-route spec,
`0x01` Sony, `0x08` generic) selects what else is made: bit 0 set (type 1) means a DS3-class device (registered with
PID `0x268`), bit 1 set (type 2) the BD remote (PID `0x306`), neither means the **generic client**: a 0x70-byte
object (constructor `0x1002C0`, factory `0x1003BC`, vtable `0x37E1A8`, client `+0x28` = the constant `0x3A`,
`+0x30` = `0x32`) registered in the singleton owner at `0x432800` (object `0x512F80`, client table at owner `+0x18`,
seven slots). **[code]** decision at `0xF76B0..0xF77FC` (vendor `0x054C` with a PID other than `0x306` counts as "not the
remote"), factory call at `0xF95E4`.

Its two readers:
- `0x10069C` (vtable slot `+0x50` of `0x37E1A8`, OPD `0x3404D8`, referenced once at `0x37E1F8`): "read request" with
  `dst`, a 16-bit length and a callback. If the data-ready flag at client `+0x68` is set it reads at once with
  `0xF8FE0(client, dst, &len, 1)` (the call at `0x1007B8`) and calls the callback; otherwise it stores `dst`
  (`+0x58`), the length (`+0x60`) and the callback (`+0x50`) as a pending request.
- `0x1000E0` (OPD `0x3403D0`, no reference): completes a pending request with the stored `dst` and length and flag 1
  (the call at `0x100130`). It is entered only by the `b` at `0x100230` from `0x1001AC`, which is called only by the HID
  layer's interrupt-in notification at `0xFA100` (under the Bluetooth lock, `0xFA040..0xFA0C4`).

The one caller of the client's read method (`0xD00C8` inside `0xCFF04`, and `0xD0144` inside `0xD012C`; receiver =
the stream object's `+0x40` field = the client) is the USB-like device framework (class vtable `0x37D288`, submit
routine `0xCE774`). It passes `len_in` = client `+0x28` = `0x3A` (58) and a `dst` taken from a staging buffer built
at `0xCFBA4`: a heap block of `0x3A + R` bytes with `dst` at offset `R`, where `R` is the largest transfer size among
the registered pipe objects. **[dump]** `ds45pad_lv2.bin`: staging block `0x660C00`, `R` = `0x4E` (78), `dst`
`0x660C4E`, length `0x3A`, so the block is 136 bytes and `dst` has 58 bytes of room (the stream request at `0x67BCA0`
and the client at `0x64D400` hold exactly these values). A 78-byte copy there would write 20
bytes past the block. **[assumed]** the heap does not round the block up enough to hide that. Only this framework
was found calling the client's read slot; the 92 `+0x50`/`+0x88` virtual-call sites in the whole image were not
all classified (the client is reachable only through the owner table, read only by `0x1001AC`, and the stream
object).

Which devices. **[dump]** In `ds45pad_lv2.bin` and `twopads/ds45pad_lv2.bin` (a stock-paired DualSense, no DS3 route)
the owner holds exactly one such client, with vendor `0x054C` and product `0x0CE6`. In `ds3route/navpid_connected.bin`
and `type01_connected.bin` (the DualSense with type `0x01`, a pad object exists) **no** object with vtable `0x37E1A8`
exists. So the DS3 and a DualSense of type `0x01` never use it; a Move controller (PID `0x03D5`), a keyboard, a mouse,
a DualShock 4 and a DualSense of type `0x08` do. **[assumed]** the Move is on it (its type word is not 1 or 2); no dump
holds a Move.

### F10.3 The transport read method `0xC9D60`, behind slot `0x37CE38` [code]

`0xC9D60(transport, dst, len_in, flag)`: stores `len_in` in a local halfword (`0x70(r1)`), loads the handle byte at
transport `+8`, and calls `0x101E20(handle, dst, &len, flag XOR 1)`. If that returns 0 it returns the halfword
(zero-extended), else it returns the error code. `0x101E20` takes the Bluetooth lock `0x4325D0`, looks the client up
(`0x1010B8`: `0x80010005` if the handle has none), calls `0xF8FE0(client, dst, &len, flag & 0xFF)` and unlocks.

`len_in` is used in exactly two ways: truncated to 16 bits and stored, and tested for non-zero inside `0xF9060`
(zero gives `0x80010002`). It is **never** compared against the record length, the read limit or the copy size.
`dst` is tested for null (`0x80010002`) and is the memcpy destination, nothing else. `flag` becomes `flag XOR 1`: the
pad class passes 0 (so the original reads twice and the second record wins if both exist), the second class passes 1
(single read).

| Situation | Ring state | `0xF9060` | `0xC9D60` returns |
|---|---|---|---|
| nothing available | at most 1 byte used (`0x1169E4` returns 0) | `*len` = 0, result 0 | **0** |
| a record that holds only the header byte | record length 1 | treated as nothing | **0** |
| a basic DualSense report | 11-byte record | copies 10 bytes from `buffer+1`, `*len` = 10 | **10** |
| a DS3 report | 50-byte record | copies 49 | **49** |
| a `0x31` report | 79-byte record | limit `0x41`: error `0x80010028` (prefix swallowed, F9.2); limit `0x50`: copies 78 | **negative today, 78 after the raise** |
| an error | closed connection, null `dst`, zero `*len`, unknown handle, record over the limit, ring copy failure | `*len` = 0 (set at `0xF91B8`), the error passed up | **negative** (`0x80010002`, `0x80010005`, `0x80010028`, `0x8001000D` seen) |

With the double read, a first read that errors returns at once (the second is skipped); a second read that errors
leaves the first record in place; the second record wins when both exist. [code]

### F10.4 What the pad driver does with 0, a negative and a positive count [code]

`0x2A4E00(pad, dst, len_in, flag)` returns 0x80010002 for a null `dst`, otherwise:

- **Negative** (`blt` at `0x2A4E78`): returns the code at once. No store to the pad object, no counter. Both outer
  callers pass it up (`0x2A5144`, `0x2A5258`); the manager wrappers return it to their caller unchanged.
- **Zero** ("no new report"): the shared steps run (the config byte `0x29E240`, the helper `0x2A6AB0` called with `dst`
  and the pad index, which takes a lock and was not analysed further, the state word at `0x4A1CD4`; all run for every
  non-negative count, so they are the same for a zero as for a real report). Then the cached-report logic runs:
  the pad object keeps its last report (a copy of up to 64 bytes at `+0x22`, length at `+0x64`, written by
  `0xC9AF0`, which clamps to `0x40`) and two flags, `+0x142` ("report seen, not yet repeated") and `+0x141` (one-shot
  set at `0x2A2490`/`0x2A3F80`). With the flag argument 0 and `+0x142` set, the cached report is copied to `dst`
  and returned once, then `+0x142` is cleared; otherwise the result is 0. The only stores are to those two cache
  flags. No disconnect, no error counter, no timer, no state change of the pad (`+8`), nothing that is not already
  done for every poll that finds the ring empty. A refused or dropped report is therefore indistinguishable from "no
  packet this poll", and the next poll reads the next record. **Dropping is harmless.** F8 corroborates it: the
  DualSense stayed connected through 100 s of errors and zeros. [code][console]
- **Positive**: if `dst[0]` is 1 and `dst[1]` is not `0xFF`, `0x2A26A8` (a button predicate that only compares the
  count with 47) and then the cache copy (clamped to 64) and the converter `0x2A4678` run on the count. A count above
  `len_in` is therefore never to be returned to this caller; the hook must return `min(count, len_in)` or 0.

### F10.5 Calling the original with a bounce buffer and `len_in` = `0x50` [code]

Safe. The original: stores `len_in` in its own frame and passes the halfword by address; tests it only for zero;
writes `dst` only with the `read - 1` byte memcpy (the call at `0xF91A8`) and `*len`; stores no pointer to `dst`
anywhere (the synchronous path keeps it in registers; only the generic client's pending request stores a `dst`, F10.2,
which is another path); has no error for a small or a large `len_in` other than 0. The pad driver cannot rely on
`dst` identity: it passes a stack local, uses the buffer only after the call returns, and the returned count is the
only link. The one requirement is the one F9.7 already states: the hook writes the translated report (49 bytes,
`dst[0]` = 1) or the passed-through bytes (at most `len_in`) into the caller's `dst`. The 96-byte bounce buffer
holds the 80-byte maximum. Concurrency: pad reads are serialised by the manager lock (F10.1), so one static bounce
buffer in the data block is not shared by two reads. **[code]** for the two manager wrappers found; the second
driver class goes through the same wrappers.

### F10.6 The poll rate in the probe run, and the DS3 [code][assumed]

**The kernel code has no back-off.** The pad read methods are driven by the pad manager wrappers (the
application's pad calls); the read path of the pad class (`0x2A4E00`, `0xC9D60`, `0x101E20`, `0xF9060`) contains no
sleep, timer or counter keyed to an error or a zero, and the manager passes the result up unchanged (`0x29EEEC`,
`0x29F418`). The only retry-with-sleep loops found nearby (`0xCE3D4`, `0xCFE24`) belong to the USB-like framework.

**The counter, not the poll, probably collapsed.** `inds` is `dbg_input_ds`, counted in `shim_input_note`, which
`shim_input` (`shim/entry.c`) calls only after its early return for `rc < 0`, `rc > 0x41`, a null pointer or a short
`len_in`. An error read (`0x80010028`) is therefore **not counted**. After the switch to the `0x31` report the pattern
in the ring is (F9.2): a fresh in-frame 79-byte record yields exactly one error read, then zero reads until the next
record; and a record arriving over an unread one resets the ring and leaves the newest record in frame. If the
DualSense's report rate is several times the application's poll rate (**[assumed]**: the `0x31` report is sent
continuously, the pad is polled tens of times a second), nearly every poll finds a fresh record and errors, and only the
rare poll that finds no new record (a zero read) is counted. A rate of about 1-2 counted reads a second is what that
model gives for a report rate three to four times the poll rate. So the "crawl" is probably the counter missing the
errors, and the polls kept running. A counter before the guard (or a separate error counter) in the next console run
would confirm it.

**The DS3 cannot be stalled by the DualSense's ring.** Each connection has its own record in the connection table
(seven entries of `0x14E8` bytes, rings at `+0x12C8 + channel*0x48`) and its own HID client object with its own ring
buffers (channel 1 at client `+0x9E`). **[dump]** `twopads/ds45pad_lv2.bin`: two table entries with two distinct
clients (`0x67FA00` and `0x67FE00`). What they share is the Bluetooth lock `0x4325D0` (held for the duration of one
read and of the notification), the shared packet buffer and the HCI transmit ring; an error read or a zero read
returns at once, so the lock is held no longer than for a normal read. The DS3's pad reads go through the same manager
lock as the DualSense's, one at a time. F8 saw the DS3 unaffected. [code][dump][console] Note: in
`ds3route/navpid_connected.bin` there is one connection and both pad objects show handle byte 0, so that dump does not
show two connections.

### F10.7 The three word edits and the DS3 and the basic-mode DualSense [code]

The immediate `0x41` is loaded into `r7` and handed down as the maximum length: `0x116EF0` (mode 2) -> `0x1170F0` ->
`0x1169E4` as its fourth argument. There it is used in **one** place: the signed compare against the record length at
`0x116D2C` (`bge` accepts, else `0x80010028`). The copy and the returned count use the record length, the ring free-space
and the used count are not compared against it (those compares belong to the send branch, F2), and nothing keeps the
value afterwards (`r7` is overwritten by the next argument setup). A 50-byte DS3 record and an 11-byte basic record are
below both `0x41` and `0x50`: same compare result, same copy, same counts (49 and 10). The clears of 65 bytes
(`li r5,0x41` before the memset at `0xF9074` and `0xF9270`) are separate immediates and stay.

Of all calls to `0x116EF0` only four are mode-2 reads with the immediate `0x41`: `0xF84D8` (channel 3, unchanged),
`0xF90E8`, `0xF918C` and `0xF924C` (channel 1: the sites to be edited). The other mode-2 calls (channel 2, at
`0xF8D04` inside the sender `0xF8BD8`, and at `0xF8FB0`) are sends; the remaining calls pass other modes (open, close, control).

**Dependency.** A read of up to 80 bytes into the shared packet buffer, plus the `buffer+1` view, needs the moved 96-byte
buffer of Amendment 1. Against the original 72-byte buffer at `0x4326C8` an 80-byte read would overwrite the first
qword of the HID client state at `0x432710` (F1.3). The three word rows must therefore never be installed without the
two buffer-slot rows.

### F10.8 Open

- A1 and A2 (above) are device facts; the Move's and the DualSense's report sizes are not in the kernel image.
- The heap rounding of the generic staging block (`0xCFC20`, allocator `0x681F4`): not traced.
- The 92 `+0x50`/`+0x88` virtual-call sites of the image were not all classified; only the framework at `0xD00C8` and
  `0xD0144` was shown to call the generic client's read slot.
- Which devices the second driver class serves (the BD remote is inferred from the 12-byte report and the type-4
  dispatch at `0x2A13CC`; the mapping from the stored type word to the factory's device type was not traced).
- Whether the application's poll rate is the one that the probe run counted (needs the extra counter, F10.6).

## F11 - Console probe run 2, with the receive path (2026-11-04, plan task 9 retry) [console]

Build: probe mode (`SHIM_OUTPUT_MODE = 1`) with the three channel-1 read-limit rows (`li r7,0x50` at 0xF9094, 0xF9134,
0xF91F4; the channel-3 limit at 0xF8484 untouched), the moved 96-byte packet buffer, the cap word `0x4D`, and the
bounce-buffer input hook for every read (R12), data block v10. Read back after install: the three limit words, the cap
word, the call site and both buffer slots as intended. Pads: a DS3 over Bluetooth and one DualSense.

**Result (passed).** The DualSense's light bar turned blue and **its input kept working** (user), the DS3 unaffected. No
hang. Counters after about 100 s: 6,407 transport reads in total, 2,946 on the DualSense, **2,917 translated**
(`xl` rising steadily), **2,916 reads longer than 65 bytes** (`inbig`: the full 0x31 reports now arrive through the
bounce buffer), `indrop` 0, `inerr` 0, `padmiss` 0, `outxl` 11 of 23 output calls rewritten. This confirms the
explanation of F8: the first rewritten output makes the pad switch to the full report, and the raised limit plus the
bounce buffer is what lets it through; the earlier stall was the over-long record desynchronising the ring.

The translated reports are full reports, so motion and the real battery level now reach the DS3 report as well.

## F12 - Console full-stage run (2026-11-04, plan task 10) [console]

Build: full mode (`SHIM_OUTPUT_MODE = 2`) with the light bar setup bytes (report byte 41 = `0x02`, byte 44 = `0x02`) and
`valid_flag0 = 0x03`. Pads: a DS3 over Bluetooth and two DualSense pads (controller numbers 2 and 3).

**How it got to work (three builds).**
- First full run: both DualSense light bars showed the pad's own blue Bluetooth start-up animation instead of the
  chosen colour (the light bar setup bytes were missing, so the pad keeps animating and ignores the colour), and there
  was no rumble (`valid_flag0` had only the compatible-vibration bit; the haptics select bit was missing).
- After adding the setup bytes the colours showed: #2 red, #3 green.
- With `valid_flag0 = 0x03` the DualSense rumbled and the player LEDs matched the number (two and three dots).

**Result (passed).** Rumble works and stops (the source was a game: a split-screen game's rumble effect reached both the
DS3 and the DualSense, and the DualSense rumbled), the player LEDs and colour follow the controller number, the DS3 is
unaffected, no freeze. A force-powered-off #3 reconnected as #3 (the stable identity of stage 1b). The DualSense input
kept working through the bounce buffer.

**Counters at the end of the run.** About 14,000 DualSense output calls, all rewritten (`outxl` = `outds`); 265
motor-on reports (`outon`) and 34 captured stops; input about 46,000 reads with about 21,000 full-length ones
(`inbig`); zero drops, errors and pad misses.

**Battery.** The raw status byte (report byte 54) was `0x00` on one DualSense (shown empty) and `0x09` on the other
(shown full). The translation is faithful to what the pads send; the level mapping is `max(1, (nibble + 1) / 2)`.

**Console restored to stock** afterwards: the hooks and the helper were verified original.

## Method (commands)

All from the scratchpad, never from the repo root (`dis.py` there shadows the stdlib), `python -I`:
`lst.py` (disassembly with TOC annotation) over `0xF8D60`, `0xF8BD8`, `0x10283C`, `0x102750`, `0x116EF0`, `0x1169E4`,
`0x115180`, `0x10B82C`, `0x11E640`, `0xCC054`, `0x2A2C30`, `0x2A2EA4`, `0xFD040`, `0xFAF08`, `0xFA9F8`;
`callers.py` plus a wider reference scan (all branch forms, descriptors, TOC slots); a TOC scan for slots with values
in `0x4325xx..0x4329xx`; a scan of the whole dump for qwords pointing into `0x4326C8..0x432717`; a connection-table
reader (table `TOC-0x2848`, 7 entries of `0x14E8` bytes, rings at `+0x12C8 + channel*0x48`) over five dumps; a pad
object reader (vtable qword `0x800000000038CDC8`) for the rumble and LED requests; the hash script.

F9 (task 11) used the same helpers plus three small scratchpad scripts: a connection-ring reader (rings at
`+0x12C8 + channel*0x48`, and the contents of channel 1), a call-site register tracker for `0x116EF0` (which channel,
mode and size each caller passes) and the hash script for the four immediates; disassembly with `lst.py` over
`0xF8FE0..0xF9280`, `0xC9D60`, `0x101E20`, `0x1169E4..0x116EF0`, `0x1170F0`, `0x11E440`, `0x11E640`, `0x11DC5C`,
`0x114DC0`, `0x10BDC4..0x10BEF8`, `0xF9284..0xF9560`, `0x2A4E00`, `0x2A50FC..0x2A5330`; `callers.py` for `0x2A4E00`,
`0xF8FE0`, `0xF91C0`, `0x101E20`.

F10 (task 12) added three small scratchpad scripts: a reference scanner (all branch forms, qwords holding a function
or its descriptor), a virtual-call scanner (a load of a given vtable slot followed within five instructions by the
`+0x10` environment load and the indirect call) and a heap-object reader for the generic client and the connection
and client tables of the connected-pad dumps; disassembly with `lst.py` over `0xF8FE0..0xF9284`, `0x100030..0x100950`,
`0xC9D1C..0xC9F30`, `0xCFBA4..0xD0330`, `0x101D98..0x101EDC`, `0x1169E4..0x116DA0`, `0x1170F0`, `0x115E80..0x115F70`,
`0x2A4D38..0x2A5330`, `0x2AB1E4..0x2AB900`, `0x2A1280..0x2A1620`, `0x29EE30..0x29F020`, `0x29F2D0..0x29F440`,
`0x2A3E60`, `0xC9AF0`, `0x2A26A8`; a scan of all `bl 0x116EF0` sites for their immediate channel, mode and size.
