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
described in prose, apart from the two single instruction words that the cap patch needs (section F5). Each fact is
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

## Method (commands)

All from the scratchpad, never from the repo root (`dis.py` there shadows the stdlib), `python -I`:
`lst.py` (disassembly with TOC annotation) over `0xF8D60`, `0xF8BD8`, `0x10283C`, `0x102750`, `0x116EF0`, `0x1169E4`,
`0x115180`, `0x10B82C`, `0x11E640`, `0xCC054`, `0x2A2C30`, `0x2A2EA4`, `0xFD040`, `0xFAF08`, `0xFA9F8`;
`callers.py` plus a wider reference scan (all branch forms, descriptors, TOC slots); a TOC scan for slots with values
in `0x4325xx..0x4329xx`; a scan of the whole dump for qwords pointing into `0x4326C8..0x432717`; a connection-table
reader (table `TOC-0x2848`, 7 entries of `0x14E8` bytes, rings at `+0x12C8 + channel*0x48`) over five dumps; a pad
object reader (vtable qword `0x800000000038CDC8`) for the rumble and LED requests; the hash script.
