# DS3-route start-up completion facts - DEX 4.92 (read-only research, plan task 1)

Scope: the facts the start-up-completion shim (spec
`docs/superpowers/specs/2026-11-01-ds45pad-ds3-route-startup-completion-design.md`, plan
`docs/superpowers/plans/2026-11-02-ds3-route-startup-completion.md`) needs about how the console's own DS3
driver reads a controller's info block, how the pad object reaches its transport, and what happens after the
start-up callback. Static analysis only, of memory dumps of the user's own DEX 4.92 console. Nothing was sent to
any console.

Sources: `analysis/ff_492.json`, `analysis/lv2_code_492.bin` (pristine kernel code, LV2 offset = file offset) and
the RAM dumps in `tools/dumps/dex492/` (git-ignored, never committed), chiefly `ds3route/navpid_connected.bin`
(two pad objects: a DS3 over Bluetooth at `0x661000` and a DualSense at `0x650800`). Cross-checked against the
other dumps that hold a DS3 pad object (`ds3route/type01_connected.bin`, `twopads/ds45pad_lv2.bin` for Bluetooth;
`nopad/ds45pad_lv2.bin`, `ds3route/rereg_ds1_on.bin` for USB).

Conventions: a bare `0xNNNN` is an LV2 offset (kernel address = `0x8000000000000000` + offset); `pad+0xNN` is a
byte offset in the 0x520-byte pad object (vtable `0x38CDC8`); `info+N` = `pad+0x80+N`. Hashes are FNV-1a 64 of the
8-aligned qword, bytes in memory order. No kernel code bytes are reproduced: instructions are described in prose.
The only dump bytes in this note are the DS3 info-block values of C1 (the per-pad identity bytes are deliberately
left out, see C1.3).

## Summary and stop conditions

| Fact | Result | Effect on the plan |
|---|---|---|
| C1 info template | The kernel reads almost every field of `pad+0x80..+0xD5`. Seven bytes (`pad+0x88..+0x8C`, `+0x8E`, `+0x8F`) are the controller's **identity**: the pad manager uses them to detect "the same controller twice" and closes the newer Bluetooth pad. Copying one fixed template into every DualSense (or a template taken from a real DS3) makes a second DualSense, or a DualSense next to the DS3 the template came from, be refused. | **STOP condition.** Per-pad bytes are required. The user must approve how the plugin/shim supplies them (proposal in C1.4). Task 3 must not start before that. |
| C2 transport embedding | `pad+0x4F0` holds a **pointer** to a separate 0x10-byte heap object, for both pads. It is not embedded. `pad = xport - 0x4F0` is wrong. The transport object has no back-pointer. | **STOP condition.** Task 3's input identity must be redesigned (options in C2.4). |
| C3 output after port assignment | Two distinct things. (a) After the compatibility check the kernel starts a **second driver** that sends a DS3 "enable" feature report `0xF4`; the port is assigned only if that driver completes with status 0 (a refusal is retried 5 times, then ignored: no port, pad kept). (b) LED/rumble output reports are sent on demand and periodically by the converter and command dispatcher; a failed send just returns an error code. No path removes the pad. | The conditional Task 4 as worded (output wrapper because a refusal can remove the pad) is **not** required. **But** stage 1 needs a second completion hook (on the `0xF4` driver's event handler), otherwise the pad object survives with no port. |
| C4 row values | All verified against the pristine code image and clean dumps. | None. |

## C1 - DS3 info template

### C1.1 Source object

The info block is `pad+0x80..+0xD5` (0x56 bytes). The pad constructor (`0x2A3ECC`) clears only `pad+0x80` and
`+0x84` (word stores) and `+0x90` (byte store); everything else in the range comes from the start-up driver's
parser (event handler `0xCACF0` and its helpers) or is leftover heap.

Reference object: the DS3 over Bluetooth in `ds3route/navpid_connected.bin`, object `0x661000` (`pad+0x6C` = 2,
VID/PID `054C/0268`). Its block is byte-identical in `type01_connected.bin` and `twopads/ds45pad_lv2.bin`. The
same DS3 over USB (clean-heap object in `nopad/ds45pad_lv2.bin`) differs only at `+0x91`, `+0x94`, `+0x95` and in
the never-written leftover region `+0x98..+0xBD`. The DualSense object `0x650800` in the same dump has all 0x56
bytes zero (its start-up never ran a parser).

### C1.2 Field map: values, writer, kernel readers

Reader addresses are in the navpid image (`navpid_connected.bin`, identical code to `lv2_code_492.bin` outside
patched words). "check" = `0x2A3AD8`, "assign" = `0x2A08A4`, "conv" = converter `0x2A4678`.

| pad offset | meaning | DS3 value (BT) | written by | read by (kernel) | decision |
|---|---|---|---|---|---|
| `+0x80` u16 | register version | `0x0005` | reg-version parser in `0xCACF0` (call `0xCAE94`, store `0xCAEAC`) | check `0x2A3B4C`: must be > 2 and != `0x817` (`0x2A3BF0..0x2A3C04`); `0x2A2810` (report class); conv `0x2A46BC` (layout select, `0x817` and `1` take other branches); report helpers `0x2A26AC`, `0x2A2714`; command dispatcher gates `0x2A36C8`, `0x2A38A4`, `0x2A3968`, `0x2A3AA8` | DS3 value |
| `+0x82` u16 | firmware version | `0x0350` | fw/identity parser (call `0xCAF14`, store `0xCAF30`) | check `0x2A3B54`: must be > `0x32F` (`0x2A3C18`); it also stores a derived copy at `pad+0x77` that assign tests > `0x2FF` (`0x2A09D4..0x2A09E8`, `0x2A20D8..0x2A20E8`); `0x2A4218` and `0x2A32A0/0x2A32F4/0x2A33D4` (second-driver start, > `0x2FF`); dispatcher gates `0x2A36A0`, `0x2A377C`, `0x2A3918` (> `0x114`) | DS3 value |
| `+0x84` u8 | device variant | `0x00` | reg-version parser (store `0xCAEB4`) | check `0x2A3B20` (values 6 and 7 take special branches); conv `0x2A46C4` (selects the motion branch); dispatcher `0x2A3694` | DS3 value (0) |
| `+0x85..+0x87` | - | 0 | - | not read | 0 |
| `+0x88..+0x8B` u32, `+0x8C` u8, `+0x8E` u16, `+0x8F` u8 | **controller identity** (seven bytes; `+0x8D` is never read) | DS3-specific | fw/identity parser (stores `0xCAF50..0xCAF78`, from the DS3's own reply) | `0x2A2514` (pad vtable +0x70): compares `+0x88`, `+0x8C`, `+0x8E` against another pad's; `0x2A2478` (vtable +0x78) returns `&pad+0x88`; `0x2A2F4C` (vtable +0x38) copies the seven bytes into the 0x1A-byte device record; check `0x2A3B04` (u16 `+0x8E` == `0x000D` plus `+0x8C` == `0x18` takes an alternate branch) and `0x2A3CDC` | **per-pad** (C1.3) |
| `+0x90` u8 | - | 0 | ctor only | `0x2A2C98` (output-report builder, first byte) | 0 |
| `+0x91` | - | stale heap (varies per dump) | not written | not read | 0 |
| `+0x92..+0x93` u16 | - | `0x018A` | fw/identity parser (store `0xCAF38`) | not read by the pad class | DS3 value |
| `+0x94` u8 | - | `0x01` (BT; 0 on USB) | 2-byte reply parser `0xCBF68` (call `0xCAE50`, writes `info+0x14/0x15`) | assign `0x2A0A68` (USB takeover branch only, via vtable +0x80 = `0x2A2480` = `&pad+0x80`) | DS3 value (BT) |
| `+0x95` u8 | - | `0x00` (BT; 1 on USB) | same | start-up driver only (`0xCAE08`) | DS3 value (BT) |
| `+0x96..+0x97` | - | 0 | - | not read | 0 |
| `+0x98..+0xBD` | - | stale heap on BT objects (zeros on the clean USB object) | **never written** | not read (the only `+0x98..` hits in the scan are vtable-slot loads) | 0 |
| `+0xBE..+0xCD` (16 bytes) | motion calibration | see C1.5 | calibration reader (`info+0x3E` at `0xCA898`) | check passes `&+0xBE` and `&+0xCE` to `0x2A2904` (builds the motion table, reads u16 at +0,+2,+4,+6,+8,+0xA and the pair at `+0xCE`); `0x2A2764` copies all 16 out (command dispatcher `0x2A36FC`); `+0xCA`, `+0xCC` read by check `0x2A3C60..0x2A3C94` (variant 6/7) and `0x2A2844` | DS3 values |
| `+0xCE..+0xD1` | motion calibration | see C1.5 | `info+0x4E` (`0xCA8C0`) | `0x2A2904` (above) | DS3 values |
| `+0xD2` u8 | capability flag | `0x01` | reg-version parser (store `0xCAEC0`) | check `0x2A3B8C` (sets a capability bit in the `pad+0x78` qword) | DS3 value |
| `+0xD3` u8 | capability flag | `0x01` | reg-version parser (store `0xCAECC`) | check `0x2A3BB0` (clears `pad+0x220/0x221`, sets a capability bit); `0x2A24D4` (vtable +0xC8); dispatcher `0x2A3700`, `0x2A3738`; conv `0x2A486C` (output pacing) | DS3 value |
| `+0xD4` u8 | flag | `0x01` | reg-version parser (store `0xCAED8`) | dispatcher `0x2A3940` | DS3 value |
| `+0xD5` u8 | flag | `0x00` | finish path | check `0x2A3B10` (non-zero on a Bluetooth pad stores 2 in `pad+0x2C1`) | DS3 value (0) |

What the three named functions require, in short:
- **check `0x2A3AD8`** (pad vtable +0x30, run from `0x2A1038`): register version > 2 and != `0x817`, firmware
  > `0x32F`, else it prints the "is old" message and returns `0x80010037` (pad removed). It also consumes the
  calibration, flags and variant bytes listed above and writes the derived `pad+0x70..+0x7F` words.
- **assign `0x2A08A4`**: for an accepted PID and firmware > `0x2FF`, Bluetooth branch: runs the duplicate
  search (C1.3), then starts the second driver (C3). It reads `+0x94` (USB only) and the derived firmware copy.
- **converter `0x2A4678`**: reads `+0x80` (layout), `+0x84` (motion branch) and `+0xD3` (output pacing). Its
  output is the DS3 report; it also sends the periodic output report (C3).

Scope note: the enumeration covers the pad class (vtable `0x38CDC8` methods and everything they call in
`0x29E000..0x2A5000`) plus the vtable methods' callers found by pattern search. A direct `pad+offset` access from
an unrelated class would not show up, and none was seen in the pad manager, driver or sender code.

### C1.3 The identity bytes are per-pad (stop condition)

How the kernel uses `pad+0x88..+0x8F`:

1. **Duplicate search `0x29E2F4(mgr, slot)`**: takes the pad's identity pointer (vtable +0x78 at `0x29E38C`), then
   asks every pad that already has a port (table `mgr+0xE0`, 16 entries) whether it matches (vtable +0x70 at
   `0x29E3FC`, the `0x2A2514` compare). It returns the slot of a match or -1.
2. **Bluetooth pad, firmware > `0x2FF`, in assign (`0x2A0B98..0x2A0BE0`)**: a match means the new Bluetooth pad
   is closed (vtable +0x98 with reason 2, callback `0x2A23F8`) and nothing else happens. No match: the pad goes on
   to the second driver. The same search runs again when the second driver completes (`0x2A05CC`), and for a USB
   pad a match closes the *older* pad instead (`0x2A0A74..0x2A0AD4`). The disconnect path `0x2A1F9C` also searches.
3. **Event export `0x29FA2C`** (called when a port is assigned, `0x2A0DE8`, `0x2A0E40`, `0x2A0818`) builds the
   0x1A-byte device record with vtable +0x38 (`0x2A2F4C`), which contains the seven identity bytes, and publishes it.

Consequence. If the shim writes the same identity into every DualSense, the second DualSense is closed as a
duplicate of the first. If the identity is copied from the user's DS3 (as a "template from a real DS3"), a
DualSense is closed whenever that DS3 is connected and has a port. Both break the stated requirement that a DS3
and several DualSense pads work together. Therefore these seven bytes **cannot** be a constant template.

The DS3's own values for these bytes are not reproduced in this note (they identify the user's controller). They
are identical across all five dumps of that DS3, which is consistent with a per-controller serial.

Constraints on a replacement value: unique among pads that are connected at the same time; not equal to a real
DS3's value; the u16 at `+0x8E` must not be `0x000D` (with `+0x8C` == `0x18` it takes the alternate branch in
check); `+0x8D` is free.

### C1.4 Proposed per-pad supply (needs the user's approval before Task 3)

The shim cannot ask the connection for an address (no calls allowed in `shim_event`), and the transport handle byte
does not tell pads apart (C2: it is 0 for every Bluetooth pad). Two options:

- **A (recommended): counter in the data block.** `shim_event` fills the identity from a fixed prefix plus a
  32-bit completion counter kept in the shim's data block, incremented once per DualSense completion. Needs
  nothing from the plugin, always unique within a boot, never equal to a real DS3's value if the prefix is chosen
  outside the DS3 range seen. Cost: a reconnecting DualSense gets a new identity, so the kernel's "same
  controller reconnected" dedupe never applies to it; harmless, as the stale pad is removed on disconnect.
- **B: plugin supplies the DualSense's Bluetooth address.** Stable per controller, but the event hook has no way
  to map its driver to an address (handle byte 0 for all, `pad+0x6D..+0x74` are zero for a DualSense), so the
  plugin would have to publish a queue of "next address" values; the spec says the plugin no longer tracks
  connections. More moving parts for no kernel-visible benefit.

Either way the template gets a per-pad copy step for these seven bytes. This is the "design addition" of the plan.

### C1.5 Template (everything except the identity bytes)

`index = pad offset - 0x80`. Identity bytes at index `0x08..0x0B`, `0x0C`, `0x0E`, `0x0F` (index `0x0D` unused) are
left zero here and must be filled per pad (C1.4). Index `0x11` and `0x18..0x3D` are leftover heap in the DS3
objects and are zero here (never read). Every other value is the DS3's, identical in the Bluetooth objects and in
the clean USB object except `index 0x14/0x15` (`0x01 0x00` for Bluetooth, `0x00 0x01` for USB; Bluetooth shown).

```c
/* DS3 info block, pad+0x80..+0xD5 (0x56 bytes), Bluetooth variant.
 * Index = pad offset - 0x80. Per-pad identity at index 0x08..0x0C, 0x0E, 0x0F: zero here, fill per pad. */
static const unsigned char DS3_INFO_TEMPLATE[0x56] = {
    0x00, 0x05, 0x03, 0x50, 0x00, 0x00, 0x00, 0x00,  /* +0x80 reg ver 0x0005, fw 0x0350, variant 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0x88 identity: PER PAD */
    0x00, 0x00, 0x01, 0x8A, 0x01, 0x00, 0x00, 0x00,  /* +0x90 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0x98 never read, never written */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xA0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xA8 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* +0xB0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFA,  /* +0xB8 (+0xBE: calibration starts) */
    0x01, 0x8E, 0x02, 0x00, 0x01, 0x91, 0x02, 0x00,  /* +0xC0 */
    0x01, 0x8D, 0x01, 0xE6, 0x00, 0x00, 0x02, 0x6D,  /* +0xC8 */
    0x02, 0x70, 0x01, 0x01, 0x01, 0x00               /* +0xD0 (+0xD2..+0xD5: flags 1,1,1,0) */
};
```

The check `0x2A3AD8` passes with these values (reg ver 5, fw 0x350, variant 0, `+0x8E` != 0xD, `+0xD5` == 0).
The calibration values are the DS3's factory-style defaults; stage 1 holds motion at the DS3 centre, so the
motion table they build is not exercised by the basic report.

## C2 - Transport embedding

### C2.1 Result: not embedded

`pad+0x4F0` holds a pointer, in both pad objects of `ds3route/navpid_connected.bin`:

| pad | `pad+0x4F0` | pointed-to object | vtable | byte at +8 |
|---|---|---|---|---|
| DS3 (BT) `0x661000` | `0x800000000067B160` | 0x10-byte heap object | `0x37CE18` | 0 |
| DualSense `0x650800` | `0x800000000067B060` | 0x10-byte heap object | `0x37CE18` | 0 |

So `xport - 0x4F0` would be `0x67AC70` / `0x67AB70`, which are not pads. The same layout (pointer to a separate
object) holds in every other dump with a pad object: Bluetooth DS3s in `type01_connected.bin` and
`twopads/ds45pad_lv2.bin` (vtable `0x37CE18`), USB DS3s in `nopad`, `rereg`, `ds45pad_lv2*.bin` (vtable `0x37CE60`).

### C2.2 Evidence from the constructor

`0x2A3ECC` (pad ctor) at `0x2A405C..0x2A407C` allocates 0x10 bytes from pool 0x22 (`0x681F4`), calls the Bluetooth
transport ctor `0xC9EF4` (vtable `0x37CE18`, stores the low byte of the device id at +8; the USB variant is
`0xC9D48`, vtable `0x37CE60`) and then stores the new object's address at `pad+0x4F0` (`0x2A407C`; the USB
branch does the same at `0x2A4104`). The pad destructors `0x2A4320`, `0x2A441C` call the object's destructor through its vtable and free it
(`0x2A4350..0x2A437C`, `0x2A444C`).

### C2.3 The transport object has no way back to its pad

Layout (16 bytes): `+0` vtable `0x37CE18`; `+8` one byte = the device id's low byte (the pad ctor's second argument,
`devid & 0xFF`); `+9..+0xF` leftover heap. There is no pointer to the pad. The byte at `+8` is 0 for **every**
Bluetooth pad (all Bluetooth pads have device id `0x0200`, USB DS3 `0x0100`), so it cannot identify a pad either;
it is used by the senders as the connection-lookup key (`0x1010B8`), e.g. by the input method `0xC9D60` (vtable
`0x37CE18` +0x20, slot `0x37CE38`: lookup via `0x101E20`, returns the length read, or a negative status).

### C2.4 Redesign options for the input identity (decision needed)

Both options validate before acting, so a wrong guess is harmless.

1. **Pad-manager table scan (no assembly tricks).** `mgr = *0x4A1CB8`; for i in 0..15, `obj = *(mgr + 0x60 + 8*i)`;
   the pad is the `obj` with `*(obj + 0x4F0) == xport`. Pure loads, bounded 16 iterations, then the existing
   vtable `0x38CDC8` and `054C/0CE6` checks. The table is the one all pad-manager code walks (`0x29E2F4`, `0x2A1038`).
2. **Caller's `r28` at entry.** The only caller found of the transport input method is one `bctrl` at `0x2A4E6C`
   inside the function starting at `0x2A4E00` (a later review found two call sites of `0x2A4E00` itself, at
   `0x2A51E4` and `0x2A5280`, both passing length `0x40`; not re-verified here), which keeps the pad in `r28` for
   the whole call (`mr r28,r3` at `0x2A4E30`,
   `lbz r0,0x21(r28)` afterwards). A wrapper entered through the transport slot sees the caller's `r28`
   untouched (`r28` is callee-saved), so a naked-asm entry could read it. Validate with `*(r28) == 0x38CDC8` and `*(r28+0x4F0) == xport`. Faster, but
   depends on this one call site's register allocation (acceptable only with the validation, and only on the
   fingerprinted kernel).

Not an option: the old plugin handle-to-slot table, because the handle byte is 0 for every Bluetooth pad.

## C3 - Output after the port is assigned

### C3.1 Sequence after the start-up callback

1. Start-up driver (ctor `0xCB1D0`) finishes; framework `0xCB5C0` calls the ready callback `0x2A122C` ->
   `0x2A1038(mgr, status, pad)`. Status != 0 removes the pad (`0x29FD8C`). Status 0 runs check `0x2A3AD8`; a
   non-zero check result removes the pad too. A passing check on a USB pad also calls `0x29FA2C(.., 0x12)`.
2. `0x2A1038` tail-calls **assign `0x2A08A4`**. For a Bluetooth pad with firmware > `0x2FF` and no duplicate
   identity (C1.3) it calls pad vtable +0xA0 (`0x2A2560`, transport +0x10: a stub returning 0 on Bluetooth) and
   then pad vtable **+0x90 = `0x2A4174`** with callback OPD `0x3663E0` (code `0x2A0844`).
3. `0x2A4174` (needs `pad+0x82` > `0x2FF`) allocates a **second driver object** (0x98 bytes, pool 0x22): vtable
   `0x37CF80`, `+0x38` state = 5 (Bluetooth; 7 for USB), `+0x3C` retry budget = 5, `+0x40` callback, `+0x48` pad,
   `+0x50` status = 0, `+0x54` pad type, `+0x55` handle byte, `+0x96` done flag. Queued with `0xCB8D8`.
4. Its send step (vtable +0x20 = `0xCAB84`, slot `0x37CFA0`) builds a 4-byte feature report whose first byte is
   `0x42` and whose second byte depends on the state (`0xCBE5C`), and sends it as **SET_REPORT, feature, report id
   `0xF4`**: Bluetooth via `0x102750` (type 3, handle byte, completion context = the driver), USB via `0x107C18`
   (request `0x3F4`). This is the DS3 "enable reporting" step, not the LED report.
5. Its event handler (vtable +0x30 = `0xCB234`, slot `0x37CFB0`, OPD `0x33A310`) stores the transfer status in
   `drv+0x50`; status 0 sets the done flag `drv+0x96` and returns 0; non-zero returns the status (done is set only
   when the budget is already 0). The framework (`0xCB5C0`) retries a non-zero return up to 5 times with a 20 ms
   sleep each, then calls the callback with the final status.
6. Callback `0x2A0844` -> **`0x2A04C0(mgr, status, pad)`**. With status != 0 it unlocks and returns: **no port is
   assigned and the pad is not removed**. With status 0 it takes a free entry of the port table `mgr+0xE0`
   (`0x2A07A0..0x2A07CC`: entry = slot, handle at `+0xF4`, count `mgr+0xF0` + 1), runs the duplicate search once
   more, and calls `0x29FA2C(mgr, slot, 1, 0x11)` which publishes the device record (vtable +0x38, C1.3) through
   `0x9DC04`. That is the port assignment.

### C3.2 LED / rumble output

The DS3 output report (LEDs, rumble) is built by `0x2A2C30(pad, request, status-out, flag)` (it needs `pad+0x22A`
non-zero) and sent by:
- Bluetooth: `0x10283C` (call at `0x2A2DE8`): wrapper that takes the Bluetooth lock `0x4325D0`, resolves the
  connection from the handle byte `pad+0x20` with `0x1010B8`, then `0xF8D60` (HID output, type 2, report `0x01`).
- USB: `0x107C18` (`0x2A2D90`) or `0x107CD8` (`0x2A2DC8`).

Triggers: the converter `0x2A4678` (calls `0x2A2C30` at `0x2A4964` and `0x2A2EA4` at `0x2A4C94`, paced by `+0xD3`
and timestamps `pad+0x218`, `pad+0x2B0`) and the pad command dispatcher `0x2A3468` (commands reach `0x2A2DF8`,
`0x2A2EA4`, `0x2A2BC8`, `0x2A2AEC`). The pad vtable +0x88 (`0x2A2488`) only records the LED request
(`pad+0xE0`, flag `pad+0x141`); the send happens from the paths above.

### C3.3 What a refused transfer does

| Transfer | Refusal handling | Pad removed? |
|---|---|---|
| Start-up requests (first driver) | retried 32 times (20 ms), then callback with the error -> removed (from earlier work, not re-verified here) | **yes** (what the spec's `shim_event` prevents) |
| `0xF4` enable (second driver) | retried 5 times (20 ms), then `0x2A04C0` ignores the error | no, but **no port is ever assigned**; the pad object stays until disconnect |
| Output report (LED/rumble) | `0x2A2C30` returns the sender's status; the converter ignores `0x8001000A` (busy) and otherwise just updates its timestamps; the dispatcher returns the code to its caller | no |

### C3.4 Decision

- A refused output report cannot remove the pad: **Task 4 as worded (output wrapper) is not required.**
- A refused `0xF4` enable keeps the pad without a port. A DualSense probably refuses it exactly like the DS3
  start-up requests (the report id does not exist for it; inferred, not observed), so the plan needs a **second
  completion hook** on the second driver's event handler slot `0x37CFB0` (original OPD `0x33A310`, code `0xCB234`,
  hash below): for a DualSense pad (`*(drv+0x48)`, same identity test as `shim_event`), store `0` in `drv+0x50`,
  `1` in the byte `drv+0x96`, return 0 -> the framework calls the callback with status 0 and `0x2A04C0` assigns the
  port. Same rules as `shim_event` (no sends, no locks, no calls apart from the tail call to the original).
- The kernel still sends the `0xF4` report to the DualSense (the shim does not add or suppress sends). If the
  DualSense never answers it (no event at all) the second driver never completes either; to be observed on the
  console (capture the second driver through `CAPTURE_PTR`).
- Open: the actual LED report goes to the DualSense in DS3 format over Bluetooth (spec 2, output, out of scope).

## C4 - Row values

Method: `ds45_fnv1a64` semantics (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`, bytes in memory
order) over the 8-aligned qwords, script in the scratchpad (not committed). Clean sources: `analysis/lv2_code_492.bin`
(pristine code) and eleven clean dumps in which the helper and the slots are unmodified (`ds3route/type01_connected`,
`ds3route/patched_ds1_on`, `ds3route/rereg_ds1_on`, `nopad`, `twopads`, `reboot_test`, `twodualsense_snap/*`, ...).
`ds3route/navpid_connected.bin` carries the manual helper patch; `shimtest/*` has the old shim's input and start-up
redirects (not clean for those two slots, clean for the event slot).

| Row | Expected | Found |
|---|---|---|
| event slot `0x800000000037CFF8` | `0x800000000033A2B0` | `0x800000000033A2B0` in every dump (also `shimtest/*`) |
| OPD `0x33A2B0` | code `0xCACF0`, TOC `0x80000000003759C0` | yes |
| event handler qword `@0xCACF0` FNV | `0x16E42A0B1EBFEFFA` | pristine image and every dump: `0x16E42A0B1EBFEFFA` |
| helper qword `@0x29E2C8`, original | FNV `0x8D1047F832C76096` | pristine image and every clean dump: `0x8D1047F832C76096` (the bytes equal the plan's original qword) |
| helper word `0x29E2CC` patched | `0x68800CE6`, qword FNV `0x8CF4E0F832AFE9D9` | computing the pristine qword with word `0x29E2CC` replaced by `0x68800CE6` gives `0x8CF4E0F832AFE9D9`; `navpid_connected.bin` (manually patched) reads exactly that |
| input slot `0x800000000037CE38` | `0x800000000033A010` | `0x800000000033A010` in every dump except `shimtest/*` (old shim redirect, expected) |
| OPD `0x33A010` | code `0xC9D60` | yes |

Additional rows for the second hook (C3.4). **Correction (final review, 2026-11-01):** the hashes first recorded
here were measured over the 8 *unaligned* bytes starting at each function's code address, not over the 8-aligned
qword the table above and the installer use (`hash_ok` hashes `addr & ~7`). All four code addresses below are
4-mod-8, so the two differ. Both are listed, recomputed on the pristine image `analysis/lv2_code_492.bin` only (the
clean dumps were not re-checked). Only the enable handler's is used by the row, and the row carries the
**aligned** value `0xB52798EFCFE165A8`.

| Row | Slot value -> code | Aligned qword FNV (installer convention) | Unaligned 8 bytes from the code address (first recorded; not used) |
|---|---|---|---|
| start-up driver vtable `0x37CFC8`: send slot `0x37CFE8` | `0x800000000033A208` -> `0xCA194` (not written by the new design) | `@0xCA190`: `0x3EDBE61A32005B76` | `0xDDCEA981EBE0056E` |
| second (`0xF4` enable) driver vtable `0x37CF80`: send slot `0x37CFA0` | `0x800000000033A298` -> `0xCAB84` | `@0xCAB80`: `0xA61224B0B4CD4E56` | `0x81AC8A3EFDF560FE` |
| same vtable, done slot `0x37CFA8` | `0x800000000033A148` -> `0xC9F3C` (reads `drv+0x96`) | `@0xC9F38`: `0x02EE22C5670E2DA4` | `0x296656446E0044E8` |
| same vtable, **event slot `0x37CFB0`** | `0x800000000033A310` -> `0xCB234` | **`@0xCB230`: `0xB52798EFCFE165A8` (the row's `enable_code_hash`)** | `0xEF8C3F93F63DFDB3` (was in the row; would have refused every install) |

The other row hashes were re-checked on the pristine image with the installer's convention and are aligned-qword
hashes as stated: event handler `@0xCACF0` (8-aligned, `0x16E42A0B1EBFEFFA`), helper `@0x29E2C8` for `0x29E2CC`
(`0x8D1047F832C76096`), inline `@0x11B5E0` for `0x11B5E4` (`0xE8ADD0EDF3B2B3C1`) and `@0x11B5E8` for `0x11B5EC`
(`0xF86140C9C3D7FB8D`); the fingerprint is the 32 bytes at the 8-aligned `0xF3380` (`0xDF192C9D78181BF7`).
`tests/test_kshim.c` (`test_builtin_row_on_pristine_image`) now runs the installer with the built-in row over the
pristine image whenever it is present locally.

## Method (commands)

Run from the scratchpad tools directory (no workspace `dis.py` there), with
`uv run --no-project --with capstone python <script> <dump> ...`:

- `padmgr.py`, `xport.py` (pad objects, `+0x4F0` pointer, transport vtable and byte, info block per dump),
- `info.py` (print `+0x80..+0xD7`), `tmpl.py` (build and cross-check the template),
- `fn.py` / `rawdis.py` / `objdump.py` (disassembly and hex ranges for `0x2A3AD8`, `0x2A08A4`, `0x2A04C0`, `0x2A1038`,
  `0x2A4678`, `0x2A4174`, `0x2A2C30`, `0x2A2F4C`, `0x2A2514`, `0x29E2F4`, `0x29FA2C`, `0xCAB84`, `0xCB234`, `0xCB5C0`),
- `scanoff.py` (all loads/stores/`addi` with a displacement in `0x80..0xD7` in `0x29E000..0x2B3000`),
- `refs.py`, `xcalls.py`, `calls18.py` (callers, OPD slots, indirect vtable calls),
- `c4.py`, `c4b.py` (hash and slot verification).
