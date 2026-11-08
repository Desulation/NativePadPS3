# DS3-route power-off facts - DEX 4.92 (read-only research, spike)

Scope: what the kernel sends or does when the user picks "Turn off the controller" for a Bluetooth DualShock 3, where
that is in the code, and whether the same thing already reaches a DualSense that the shim presents as a DS3. Static
analysis only, of the pristine kernel code image and the memory dump of the user's own DEX 4.92 console. Nothing was
sent to any console or network. Spec: `docs/superpowers/specs/2026-11-04-ds45pad-ds3-route-poweroff-design.md`.

Sources: `analysis/lv2_code_492.bin` (pristine kernel code, 3 MiB, LV2 offset = file offset, in the parent folder of
the repo checkout) and `tools/dumps/dex492/ds45pad_lv2.bin` (RAM dump; every site named below is byte-identical to the
pristine image, checked for each hash row).

Conventions as in `2026-11-03-ds3-route-output-facts.md`: a bare `0xNNNN` is an LV2 offset (kernel address =
`0x8000000000000000` + offset); `pad+0xNN` a byte offset in the pad object (vtable `0x38CDC8`); `obj+0xNN` a byte offset in
the object named in the sentence; hashes are FNV-1a 64 of the 8-aligned qword, bytes in memory order. No kernel code
bytes are reproduced. Each fact is marked **[code]** (read in the kernel image), **[dump]** (read in the dump),
**[inferred]** (follows from code but a step was not read) or **[assumed]** (device behaviour, outside the kernel).

## Summary

| Question | Answer |
|---|---|
| Does the kernel ever send a Bluetooth HID_CONTROL message (header `0x10` OR n, e.g. VIRTUAL_CABLE_UNPLUG = `0x15`)? | **No.** Every writer of the HID L2CAP channels is enumerated (P1): exactly three packet builders exist (DATA output `0xA0`+type, SET_REPORT `0x50`+type, GET_REPORT `0x48`+type). No code anywhere builds a `0x1n` header. [code] |
| What does the pad-side "close the pad" request send to a BT DS3? | One **SET_REPORT, feature type, report id `0xF4`, 4 data bytes `42 08 00 00`** (control channel, L2CAP header byte `0x53`). It comes from pad-manager ioctl command 6 (privileged), via pad vtable `+0x98`. After it the kernel unassigns the pad's port and publishes a "port removed" event; it does **not** disconnect the Bluetooth link. [code] |
| Is that the "Turn off the controller" request? | **Not provable statically.** It is the only pad-side request in the image that ends in "tell the controller something and release the port", it is privileged like the other VSH-type commands, and its payload (feature `0xF4`, all enable bits clear) fits "stop". Which syscall the XMB uses was not found. [inferred] |
| Does the DualSense already receive it? | **Yes, if the request is that one.** Nothing in the path looks at VID/PID or the DS3 info block except two gates the shim already satisfies (pad firmware word `pad+0x82` > `0x114`, bus type `pad+0x6C` == 2) and the VID/PID helper `0x29E2B4` (already patched to accept `0x0CE6`). The DualSense receives `F4 42 08 00 00` and has no reason to power off on it. [code][inferred] |
| Which sender? | Control-channel sender `0xF8BD8` through the wrapper `0x102750` (call site `0xCA9BC`), **not** the interrupt sender `0xF8D60` and **not** the wrapper `0x10283C`. The shim's output hook at `0x2A2DE8` (`bl 0x10283C`) cannot see it. `0xF8BD8` has no 64-byte cap. [code] |
| Other way the kernel ends a BT link | `0x1174FC(bd address, flag)` (graceful flag: L2CAP Disconnection Request first; flag 0: HCI Disconnect at once) and the HCI emitter `0x112BE0` with opcode `0x0406`, reason `0x13`. No pad-class code calls them (P3). The only syscall found that reaches `0x1174FC` also deletes the stored pairing entry (not a plain "turn off"). [code] |
| Smallest hook | Replace the `bl 0x102750` at `0xCA9BC` for DualSense pads when `r4`=3, `r5`=`0xF4`, `obj+0x38`=3: send the DualSense power-off feature report instead of `F4 42 08 00 00` through the same wrapper, same cookie and context (P6). Observe first (passive counters at the same site) to confirm the XMB action really reaches it. |

## P1 - What can be written to the HID channels (complete list) [code]

All 27 `bl`/`b` to the L2CAP channel entry `0x116EF0(addr, channel, mode, buffer, length)` were enumerated, plus
the sibling `0x116D8C` (2 callers) and the lower entries `0x1169E4` (callers `0x116E60`, `0x116ECC`, `0x11710C`,
`0x11BCB8` - all inside `0x116D8C`, the mode-2 handler `0x1170F0` and a debug reader). Mode is the third argument
(register `r5`, after address and channel); the dispatch is the jump table at `0x116FE4`, modes 0..12.

Packet writers (a byte string is composed in a buffer, then handed down as mode 2):

| Writer | Header byte | Channel | Content | Caller chain |
|---|---|---|---|---|
| `0xF8D60` (call `0x116EF0` at `0xF8E94`, tail) | `0xA0` OR type (DATA, type 2 = output) | 0 (interrupt out) | report id + payload, payload <= 64 (`cmplwi 0x40` at `0xF8D74`) | `0x10283C` (only caller `0x2A2DE8`), generic `0x100564` |
| `0xF8BD8` (call at `0xF8D04`) | `0x50` OR type (SET_REPORT; type 3 = feature gives `0x53`) | 2 (control out) | report id + payload, 16-bit length, **no cap**; sets the busy byte `client+0x167` until the handshake, stores cookie/context at `client+0x178/+0x180`; returns `0x80010001` if still busy | `0x102750` (callers `0xCA0F0`, `0xCA334`, `0xCA9BC`, `0xCACEC`), generic `0x100904`, and the wrapper `0x102750` itself at `0x1027F0` |
| `0xF8E98` (call at `0xF8FB0`) | type OR `0x48` (GET_REPORT with size) | 2 | 4 bytes: header, report id, 16-bit size (composed on the stack) | `0x102908` (callers `0xCA184`, `0xCA260`, `0x2AAEA8`) |

Everything else that reaches `0x116EF0` has a mode other than 2-with-a-header: open (mode 0: `0xF8338`, `0xF9368`, `0xF93F0`,
`0xF9478`, `0xF94EC`, `0xFA2B0`), close (mode 1: `0xF9AC0`, `0xF9C08`), receive reads (mode 2 on channels 1 and 3: `0xF84D8`,
`0xF90E8`, `0xF918C`, `0xF924C`), state/flush (modes 3, 4, 5: `0xF7900`, `0xF79A8`, `0xF83E4`, `0xF8B04`, `0xF8BAC`, `0xF9A4C`,
`0xF9B94`, `0xFA35C`) and port configuration for the Bluetooth audio code (modes 6, 8, `0xB`, `0xC`: `0xF7D74`, `0xF7E68`,
`0xF8688`, `0xF8738`, `0xF87C0`, `0xF8894`; the error strings at those sites say `bt_port_control(... SET_CONF / SET_GAIN)`).
The labels of modes 3-6, 8, `0xB`, `0xC` are [inferred] from the call-site arguments (lengths 0 or 4/10, no header composed).
The mode 0, 1, 2, 4 handlers (`0x117054`, `0x117018` -> `0x116708`, `0x1170F0`, `0x117034` -> `0x116894`) were read.

`0x116D8C` sends the caller's buffer unchanged on channel 0 (flag 1 into `0x1169E4`) with no header of its own: its only
callers are `0xF8954` (channel 1) and `0xF8A30` (channel 0), reached through client wrappers `0x101CC0` / `0x101900`
whose callers sit in the Bluetooth audio code (`0x1065xx` uses 8000 Hz constants). [code] for the chain, [inferred] that
it is audio only. A pad never goes through it.

A scan of `0xF4000..0x105000` for `li`/`ori` with `0x10`, `0x11`, `0x13`, `0x14`, `0x15` finds only unrelated hits (flag
bits at `0xF7B64`, `0xF7C7C`; sample-rate code; event numbers `0x15` at `0xFAAE4` etc. that are arguments of the event
function `0xFA6A8`). No header `0x1n` is ever stored in a packet. [code]

Consequence: a real DS3 cannot be powered off by VIRTUAL_CABLE_UNPLUG from this kernel. Whatever makes it power off is
one of: a feature/output report from P2, the link teardown of P3, or the controller's own reaction to either.

## P2 - The pad-side close request [code] (except where marked)

Chain, from the privileged ioctl down to the wire:

1. **Pad-manager ioctl `0x2A1B90`** (manager vtable `0x38CD40` slot `+0x38`; arguments: manager, port, command, buffer,
   length, extra). A 12-entry command table at `0x38CDA0` (command byte, flag byte) lists 0, 1, 2, 3, 4, 5, 6, 7, 8, `0xB`,
   `0x64`, `0x65`; commands 0, 4, 6, 7, 8 carry flag 1. For flag 1 the call needs the check function `0x3560(.., "pad_manager"
   string at 0x315C40, ..)` to return non-zero, else the ioctl returns `0x80010003` (`0x2A1E7C..0x2A1E9C`). The nature of the
   check (process privilege) was not traced; it is the same function the media-key syscall uses with its own name string.
   [code] for the gate, [inferred] that it is a VSH-only privilege.
2. **Command 6 branch** (`0x2A1D34` compares 6, branch to `0x2A1EE8`): takes the manager lock off, calls
   **`0x2A19D4(mgr, port, pad+0x68)`**. The block at `pad+0x68` is VID (`+0`, `+1`), PID (`+2`, `+3`) and the bus type
   (`+4` = `pad+0x6C`: 1 USB, 2 Bluetooth).
3. **`0x2A19D4`**: calls **pad vtable `+0x98` = `0x2A3280`** (call at `0x2A1A44`) with `r4` = bus type (`lbz 4(r31)`) and
   callback OPD `0x366500` (code `0x2A23F8`). If the manager's primary-port byte `mgr+0x3A` equals this port and `mgr+0x3B`
   is not `0xFF`, the same call is repeated for the pad at `mgr+0x3B` (`0x2A1B48`). Then, if `0x29E2B4(VID, PID)` accepts
   the pad and its bus type is 1 or 2, it calls **`0x2A0300(mgr, port)`**.
4. **`0x2A3280`** (needs `pad+0x82` > `0x114`, else `0x80010037`): for `r4` == 2 the kind is 3, for `r4` == 1 the kind is 6;
   Bluetooth pads (`pad+0x6C` == 2) take the branch at `0x2A33B0..0x2A344C`: allocates a `0xA0`-byte command object (pool
   `0x22`), vtable `0x37CEF0`, `obj+0x38` = kind, `obj+0x3C` = 5 (retry budget), `obj+0x40` = callback, `obj+0x48` = pad,
   `obj+0x94` = handle byte (`pad+0x20`), `obj+0x96` = `pad+0x82`, `obj+0x98` = 1 (state "feature step"), and queues it with
   `0xCB8D8`. USB pads use vtable `0x37CF38` and start in state 0 for kind 6.
   For a Bluetooth DS3 (or a DualSense seen as one) the manager passes bus type 2, so **kind 3**.
5. **Run method `0xCA90C`** (vtable `0x37CEF0` slot `+0x20`): state 1 requires kind 3 (any other kind returns
   `0x8001000C`), then `0xCBE5C(obj+0x54, 0x40, 8, 0x42)` builds the 4-byte payload **`42 08 00 00`** (byte 0 = argument 4,
   byte 1 = argument 3, bytes 2 and 3 zero) and the shared call site **`0xCA9BC`** calls
   **`0x102750(obj+0x94, 3, 0xF4, obj+0x54, 4, cookie, obj)`** (registers `r3`..`r9`). [code]
6. **`0x102750`** takes the Bluetooth lock `0x4325D0`, resolves the client with `0x1010B8`, and calls **`0xF8BD8`**: header
   `3 | 0x50` = **`0x53`**, report id `0xF4`, payload copied after it, `length + 2` bytes written to channel 2 (control out).
   The wire packet is `53 F4 42 08 00 00`. The control-channel TX ring holds 67 bytes (F2 of the output note), so it fits.
7. **Completion**: the handshake reaches the command object's event handler `0xCB114` (vtable `+0x30`), which stores the
   status; status 0 moves the state to 2 (done), a non-zero status is returned to the framework `0xCB5C0`, which retries
   (budget `obj+0x3C` = 5, 20 ms apart, per C3.1 of the start-up note) and finally calls the callback `0x2A23F8` ->
   `0x2A2280(mgr, kind, status, pad)`. For kind 3 that function does nothing further for a Bluetooth pad (its only action is
   for kind 6 on a USB pad: pad vtable `+0xA8`).
8. **`0x2A0300`** (after the send was queued): searches the port table `mgr+0xE0` for the port, writes `0xFF` to that entry,
   decrements `mgr+0xF0`, calls pad vtable `+0x20` (`0xC9A10`: clears the open count), and publishes the event
   `0x29F8E0(mgr, port, 0x11)` (the "port removed" record that libpad and the VSH read).

What this chain does **not** do: no HCI or L2CAP disconnect, no call into `0x1174FC` / `0x10268C` / `0x112BE0`, no
interrupt-channel output report. The pad object (and its HID client) survive until the Bluetooth link itself ends.
The pad class never calls any disconnect function anywhere (callers of `0x10268C` are `0xE1424`, `0xE1A44`, `0xE3C54`
in the HID-core/gamepad code and `0x2AABD8`, `0x2AB4CC` in the second driver class = BD remote; none in `0x2A0000..0x2A5000`
or `0xC9000..0xCC000`). The pad's own command dispatcher `0x2A3468` handles only 0..8, `0xB`, `0x66`; its command 6 is
`li r3,0` (`0x2A3674`), the work is in the manager (step 2).

### The `0xF4` payload family [code]

`0xCBE5C` builds `42 mask 00 00`. The masks seen in the pad code:

| Sender | Object kind (`obj+0x38`) | Mask | Meaning |
|---|---|---|---|
| second driver `0xCAB84` (start-up "enable", vtable `0x37CF80`), table at `0xCABD8` | 3, 4, 5, 6, 7, 8 -> | `08`, `09`, `0A`, `0B`, `0C`, `0D` | the console starts a Bluetooth DS3 with kind 5 (`0A`) and a USB DS3 with kind 7 (`0C`) |
| command object `0xCA90C` (this note, BT) | 3 | `08` | the close request: neither the Bluetooth nor the USB enable bit |
| command object `0xCAA3C` (USB) | 3 or 6 | `08` or `0B` | USB variants; kind 6 first sends a zero output report (state 0) |

[inferred] The USB value `0C` is the widely used DS3 USB enable mask, so bit `0x04` looks like the USB enable and `0x02`
the Bluetooth enable; the close request is the same report with both enable bits clear. What a real DS3 does on that
report (disable reporting, disconnect, power off) is device behaviour and **[assumed]** here.

### Where a pad can end up powered on, disconnected or removed after this request [inferred]

The kernel itself sends nothing more to the controller. If a real DS3 ends up powered off, it is either its own reaction
to `F4 42 08 00 00` or a link teardown by a different request (P3). The observation that a DualSense "only disconnects"
means that something ends the link for it; the pad code in P2 does not, so either the DualSense drops the link by itself
after the refused report (it does refuse `F4`, see C3.4 of the start-up note) or the VSH issues a Bluetooth disconnect as
well (P3). A passive log at the sites in P6 tells which.

## P3 - Bluetooth link teardown entries [code]

| Function | Role | Callers |
|---|---|---|
| `0x1174FC(bd, flag)` (`r3` = 48-bit address, packed as bytes 6, 7, 4, 1, 2, 3 of the 8-byte address struct; `r4` = flag) | Finds the connection (`0x1125F4(5, bd)`), refuses if the "closing" bit `0x4000` is set, sets it. Flag 0: with an ACL handle (`conn+0x28` != 0) sets bit `0x8000` and sends **HCI Disconnect (opcode `0x0406`, handle, reason `0x13` = remote user terminated)** at once through `0x112BE0` (`0x1176D0`); without a handle sends opcode `0x0408` (create-connection cancel). Flag != 0 (graceful): after `0x10D344`/`0x11361C`, looks up the interrupt channel (`0x1127B8(handle, 0x23, 0x13)`, else the control channel with `0x11`) and sends an **L2CAP Disconnection Request** (`0x10BA24` -> `0x10B82C` with command code 6, length 4); the HCI disconnect follows from the L2CAP state machine (not traced). | `bl` at `0xF4D94` (BT control syscall, see below), `b` at `0xF722C` (inside `0xF71E4`, which `0x10268C` calls with the flag inverted), `bl` at `0xFDB44` (inside `0xFDAB8` = HID client vtable `0x37DFA0` slot `+0x68`, flag 1), wrapper `0x117708` (flag 1, no caller found) |
| `0x117744(bd)` | Cancel/disconnect: opcode `0x0408` when there is no handle, else L2CAP Disconnection Request, else opcode `0x0406`, reason `0x13` | `0xFC020`, `0xFDC04` (the `bt_connect_cancel` log string is at the second) |
| `0x112BE0(opcode, format, args)` | The HCI command formatter. Opcode `0x0406` is passed at 13 sites (`0x10B03C`, `0x10C3C8`, `0x10C788`, `0x10C800`, `0x10D140`, `0x11121C`, `0x113CB4`, `0x113FA8`, `0x114C18`, `0x1176D0`, `0x117844`, `0x117E00`, `0x11C690`); reasons seen: `0x0D`, `0x13`, `0x14`. | internal to the L2CAP/HCI code (timeouts, errors) as well as the two functions above |
| `0x10268C(handle byte, flag)` | Lock `0x4325D0`, find the entry in the handle table (`0x432690`, via `0xF719C`), call `0xF71E4(entry, flag)` -> `0x1174FC(addr, flag XOR 1)` | `0xE1424` (flag 1: "disconnect by host" after a failed get/set feature in the gamepad ioctl), `0xE1A44` (flag 0, failed device registration), `0xE3C54` (gamepad device ioctl command 6, flag 0, Bluetooth only), `0x2AABD8`, `0x2AB4CC` (BD-remote class) |

All of `0xE1xxx..0xE6xxx` belongs to the HID-core/"HidGamepadMgr" class (name strings at `0x2FB7A0`, `0x2FC678`), whose device checks
use VID `0x054C` with PID `0x03D5` (the Move controller) and whose syscall is table entry 648 (below). A DS3 pad object is not
a device of that class. [code] for the checks, [inferred] for the conclusion.

**BT control syscall**: the syscall table is the run of 1051 code descriptors at `0x38A410` (entry n = qword `0x38A410 + 8n`;
slots 8..26 hold non-pointer data, so the numbering is anchored on the plausible neighbours 1..7 and 27+ only [inferred]).
Entry 662 is `0xF4B68` (`cmd` argument 0..28, jump table `0xF4C18`). **Command `0x16`** (`0xF4CF0`): copies 8 bytes from the
user, calls `0xF6174` -> `0x1162FC(addr, 0, 0)` -> `0x115FDC`, which sets the stored-device entry's type word (table
`0x434910`, 32 entries of 32 bytes, type word at `+8`) to 0 = frees the entry, then **`0x1174FC(addr, 1)`**, then posts
event 9 to the HID host client (`0xFB4F0`). It runs under the Bluetooth lock `0x4325D0` (taken at `0xF4BE4`). So it
unpairs and disconnects; the XMB "Turn off controller" would not use it, because the next PS-button press must still reconnect.

## P4 - Dependence on the controller's identity

- P2 steps 1-4: no VID/PID or DS3-info compare except: the table flags (command number), `pad+0x82` > `0x114` (the shim's DS3
  info block carries `0x0350`), `pad+0x6C` == 2, and the VID/PID helper `0x29E2B4` in `0x2A19D4` (gates only step 8, the port
  unassign; the PID patch makes it accept `0x0CE6`). Same results for a DS3 and for a DualSense seen as a DS3. [code]
- The HID client class for a DualSense is chosen by the stored type word (type 1 -> DS3-class client; a stock-paired DualSense
  with another type uses the generic client and has no pad object at all); with the manual type flip the connection is
  handled as a DS3. [code from F10.2 of the output note]
- P3: keyed by Bluetooth address and connection state only; no identity dependence. [code]

## P5 - Which hook sees what

| Message | Sender | Wrapper | Seen by the output hook at `0x2A2DE8`? |
|---|---|---|---|
| Output report 1 (rumble, LED) | `0xF8D60` | `0x10283C` | yes (that is the hook) |
| `F4` close request (P2) | `0xF8BD8` | `0x102750`, call site `0xCA9BC` | **no** |
| `F4` start-up enable | `0xF8BD8` | `0x102750`, tail `b` at `0xCACEC` | no |
| Start-up zero output report, feature `0xEF` | `0xF8BD8` | `0x102750` (`0xCA334`, `0xCA0F0`) | no |
| Feature/GET reports `0xEF`, `1` | `0xF8E98` | `0x102908` (`0xCA184`, `0xCA260`) | no |
| Link teardown | none (signalling) | `0x1174FC` | no |

## P6 - Hook candidates for "turn the request into a DualSense power-off"

Contract at the shared call site `0xCA9BC` (inside `0xCA90C`, reached with `r30` = command object):

| Register | Value |
|---|---|
| `r3` | handle byte (`obj+0x94`) |
| `r4` | 3 (feature type); 2 would be the zero output report of state 0, never used on Bluetooth |
| `r5` | `0xF4` |
| `r6` | `obj+0x54`, a 0x40-byte payload buffer inside the object (ends at `obj+0x93`, the handle byte follows) |
| `r7` | 4 |
| `r8` | completion cookie (the run method's second argument) |
| `r9` | `r30` = command object (`obj+0x38` kind, `obj+0x48` pad) |

1. **Smallest hook: the `bl 0x102750` at `0xCA9BC`** (the instruction pair `lbz r3,0x94(r30)` / `bl` is the qword at `0xCA9B8`).
   A stub entered there tests `r4` == 3, `r5` == `0xF4`, `obj+0x38` == 3 (this excludes the second driver, whose send is a
   different instruction `0xCACEC`) and the DualSense test on `*(obj+0x48)` (the same pad-identity test as the shim's
   completion hook), and for a DualSense replaces the arguments: payload buffer `obj+0x54` (64 bytes of room) filled with the
   DualSense power-off feature report, `r5` = that report's id, `r7` = its length, then branches to `0x102750` (a tail call keeps
   cookie and context, so the command completes normally and the retry/callback logic is unchanged). Everything else tail-calls
   `0x102750` unchanged. The Bluetooth lock is taken inside `0x102750`; the stub holds no lock and calls nothing else.
   The packet then goes out through the same control-channel sender with header `0x53`.
   Unverified device side (from the third-party `dualsensectl`, as given): feature report `0x08`, payload `0x02`, 47 bytes on
   Bluetooth, CRC-32 over the seed byte `0x53` followed by the report, little-endian, in the last four bytes. The seed `0x53` is
   exactly the header the kernel puts in front of a type-3 SET_REPORT (`0x50` OR 3), which is consistent with that description.
2. **Alternative / second site: the link teardown** at `0x1174FC` (entry), or its three call sites. All API-level disconnects pass
   there with `r3` = address and `r4` = flag, but the stub would have to map the address to the handle byte (handle table
   `0x432690`: 7 qword slots, each pointing to an object whose byte `+0x10` is the handle byte and whose qword `+0x18` leads to
   the connection record with the address, per the stage 1b identity note) before it can use `0x102750` (which takes the
   handle byte). It runs under the Bluetooth lock in the syscall path, so it must call the
   unlocked `0xF8BD8` with the client pointer, not `0x102750`; `0xF8BD8` takes the HCI lock `0x112EC8` inside `0x116EF0`, which
   `0x1174FC` has not yet taken at its entry. Hooking `0x112BE0` (opcode `0x0406`) would see the internal paths too but runs with
   the HCI lock held and sees an ACL handle, not an address: not recommended.
3. **Do not** use the output hook at `0x2A2DE8`; it only sees output report 1.

### Hash rows (FNV-1a 64 of the 8-aligned qword; pristine image and dump identical)

| Site | Aligned qword | Content | FNV-1a 64 |
|---|---|---|---|
| shared send call in the close run method | `0xCA9B8` | `lbz r3,0x94(r30)` + `bl 0x102750` | `0x6D3B5A2728211E68` |
| second driver send (start-up `F4`) | `0xCACE8` | `addi r1,..` + `b 0x102750` | `0x1C175B6DAC543D89` |
| wrapper entry | `0x102750` | `stdu r1,-0xD0(r1)` + `mflr r0` | `0x5349C3CAC4F910BE` |
| sender entry | `0xF8BD8` | `stdu r1,-0xC0(r1)` + `mflr r0` | `0xDDCEA981EBE0056E` |
| pad vtable `+0x98` function entry | `0x2A3280` | `stdu r1,-0xC0(r1)` + `mflr r0` | `0xDDCEA981EBE0056E` |
| close run method entry | `0xCA908` (tail of the previous function + `stdu`) | | `0xF4C9E2458F5C1E36` |
| payload packer entry | `0xCBE58` | | `0x05EDC6C637858BC7` |
| manager command 6 call `bl 0x2A19D4` | `0x2A1F08` | `li r29,0` + `bl` | `0xF7B8364ADD77C635` |
| port unassign entry | `0x2A0300` | | `0x5D4E4578D571F2AE` |
| teardown API entry | `0x1174F8` | previous function's last branch + `stdu` | `0x3B9B7B10185C9776` |
| teardown call, BT control syscall | `0xF4D90` | `bl 0x1174FC` at `0xF4D94` | `0x28861307B803CC8E` |
| teardown call in `0xF71E4` | `0xF7228` | `b 0x1174FC` at `0xF722C` | `0x2965949581F9EB53` |
| teardown call, HID client `+0x68` | `0xFDB40` | `bl 0x1174FC` at `0xFDB44` | `0x10FC449574975856` |
| HCI command formatter entry | `0x112BE0` | | `0x272A0358EF9E3734` |
| `0x10268C` entry | `0x102688` | | `0xF1E5744AC121573C` |

The two `0xDDCEA981EBE0056E` rows have identical bytes at different addresses; an installer keys the check by address.

## P7 - Not knowable statically

- Which request the XMB "Turn off controller" issues (pad-manager ioctl 6, a Bluetooth disconnect, or both). Needs one passive
  capture: counters/last-arguments at `0xCA9BC` (kind, report id, mask byte) and at the `0x1174FC` entry (flag) around the menu
  action, read back after the fact (no output, no prompt).
- What a real DS3 does on `F4 42 08 00 00` (power off, or only stop reporting) and on the L2CAP/HCI disconnect (device firmware).
- Whether the DualSense accepts a feature SET on report `0x08` over Bluetooth, the exact report length and CRC rule, whether it
  answers with a success handshake, and whether it powers off while the kernel's own teardown (if any) follows. A dropped link
  before the report is processed is possible when the teardown path of P3 follows immediately; the L2CAP packets of one ACL link
  are in order, so the report arrives first, but the pad's firmware order is not known.
- Whether the DualSense's observed "disconnect" is its own reaction to the refused `F4` (retried up to 5 times) or a kernel
  disconnect (P3) that the VSH issues in addition.
- The state after the close command for a USB pad (kind 6 path) - irrelevant for the shim, not traced to the end.
- The privilege check `0x3560` for commands with flag 1 (which process passes), and which syscall number leads to
  `0x2A1B90` for a user request: the manager is reached through the HidPadWrapper class (`0xE65E8`, vtable `0x37DD28` slot
  `+0x38`, port argument bit `0x40` selects the Move manager `0xE0F5C` instead of the pad manager `0x29E88C`); the syscall
  entry above it (table entries 647/648 are the nearest, 648 = `0xE5C10` has 13 commands) was not matched to command 6.

## Method (commands)

All from the scratchpad, never from the repo root, `python -I`, capstone with a word-wise disassembler (`dis2.py`) that keeps
going over data words: ranges `0xF4000..0x105000`, `0x105000..0x120000`, `0x29E000..0x2A2400`, `0xC9D00..0xCACF4`,
`0xCB114..0xCB2C0`, `0xCBE5C..0xCBF80`, `0x2A2AEC..0x2A3C00`, `0x2A3280..0x2A3468`, `0x2A19D4..0x2A2000`, `0x2A0300..0x2A04C0`,
`0x2A2280..0x2A246C`, `0x1174FC..0x117860`, `0x112BE0..0x112DD8`, `0x10B..` for `0x10BA24`, `0x115FDC..0x1162FC`, `0xE13xx..0xE6xxx`
(HID core), `0xF4B68..0xF5400` (BT control syscall). A cross-reference index of every direct `b`/`bl` target in `0..0x300000`
and a descriptor (OPD) finder (code address -> OPD -> qwords that hold it) gave the caller lists; the vtable scanner of F10
(`ld slot(rB)` followed by `ld 0x10(rA)` and `bctrl`) gave the manager and pad vtable callers; the syscall table was found as
the 1051-entry run of descriptor pointers at `0x38A410`; strings were listed with a printable-run scan of the dump. Hashes:
`ds45_fnv1a64` semantics (basis `0xCBF29CE484222325`, prime `0x100000001B3`, bytes in memory order), computed on both images.
