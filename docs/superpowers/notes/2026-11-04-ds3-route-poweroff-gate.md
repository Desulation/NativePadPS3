# DS3-route power-off: gate note

Gate for `docs/superpowers/plans/2026-11-04-ds3-route-poweroff.md` Task 1. Section G0 was written by the controller from a research subagent's report; G1-G6 follow below.

## G0: the DualSense Bluetooth power-off report (third-party source)

Source: `nowrep/dualsensectl`, `main.c` and `crc32.h` on branch `main` (head at fetch time `d3ae2fa`, 2026-02-19; the file was not pinned to that commit). The subagent read the files through a page summariser, so the quoted names are close to verbatim, not byte-checked. Linux `hid-playstation.c`, SDL `SDL_hidapi_ps5.c` and pydualsense contain no power-off code, so `dualsensectl` is the only source; its claim has not been checked on hardware.

| Item | Finding | Matches the spec |
|---|---|---|
| Report id | `0x08` (`DS_FEATURE_REPORT_BLUETOOTH_CONTROL`) | yes |
| Kind | feature report (`hid_send_feature_report`), no output report | yes |
| Length | 47 bytes including the id (`..._SIZE 47`) | yes |
| Set bytes | `buf[0] = 0x08`, `buf[1] = 2` (`DS_BLUETOOTH_CONTROL_OFF`), rest zero, bytes 43-46 the CRC | yes |
| CRC | reflected CRC-32 (poly `0xEDB88320`), init `0xFFFFFFFF`, final inversion; seed byte `0x53` then `buf[0..42]` (43 bytes) | yes |
| CRC position | bytes 43-46 (`sizeof(buf) - 4`), stored as a native `uint32_t` (little-endian on x86/ARM; inferred, not documented) | yes |
| Bluetooth only | CRC computed only when the device is Bluetooth; over USB the same buffer is sent with zero CRC bytes; help text says "BT only" | n/a (we use Bluetooth) |

Not confirmed: byte-exact quotes, the exact commit, how the tool decides the device is Bluetooth, and whether the controller really powers off on this report. The console act stage settles the last one.

G0 verdict: matches the spec on every point; the plan continues.

## Method (G1-G6)

Static analysis of the two images only; nothing was sent to the console. Images: the pristine kernel code image (3 MiB, LV2 offset = file offset, kernel address = `0x8000000000000000` + offset) and the RAM dump of the user's DEX 4.92 console (8 MiB; the dump is indexed by the same offsets, and holds the data pages such as vtables and the TOC at `0x3759C0`, which the code image lacks). Addresses below are LV2 offsets; `obj+0xNN` is a byte offset in the command object (vtable `0x37CEF0`), `pad+0xNN` in the pad object. FNV-1a 64 is the `ds45_fnv1a64` definition of `src/lv2.c` (basis `0xCBF29CE484222325`, prime `0x100000001B3`, bytes in memory order) over the 8-aligned qword. Disassembly was read with a capstone helper; only addresses, mnemonics and hashes are recorded here. Caller lists come from an index of every direct `b`/`bl` in `0..0x300000`, plus a search of the dump for descriptors (OPDs) and for the code addresses held as data.

## G1: the four sites, their qwords and hashes

| Site | Decodes as | Half of its qword | Other half | Qword | FNV-1a 64 pristine | FNV-1a 64 dump | Constraint row |
|---|---|---|---|---|---|---|---|
| `0xCA9BC` | `bl 0x102750` | second (offset +4) | `lbz r3,0x94(r30)` at `0xCA9B8` | `0xCA9B8` | `0x6D3B5A2728211E68` | same | `0x6D3B5A2728211E68` |
| `0xF4D94` | `bl 0x1174FC` | second | `extsw r31,r30` at `0xF4D90` | `0xF4D90` | `0x28861307B803CC8E` | same | `0x28861307B803CC8E` |
| `0xF722C` | `b 0x1174FC` | second | `or r3,r0,r3` at `0xF7228` | `0xF7228` | `0x2965949581F9EB53` | same | `0x2965949581F9EB53` |
| `0xFDB44` | `bl 0x1174FC` | second | `or r3,r0,r3` at `0xFDB40` | `0xFDB40` | `0x10FC449574975856` | same | `0x10FC449574975856` |

Each qword is 8-aligned and each site is the second word of its qword. The site words are byte-identical in both images (same hash). Branch targets in the qwords: the first half of `0xCA9B8` is itself a branch target (a conditional branch from `0xCAA34`, the state-1 path), the site `0xCA9BC` is reached only by falling through from `0xCA9B8`; no branch lands on `0xCA9BC` directly.

Verdict G1: **CONFIRMED** (all four decodes and all four hashes, both images).

## G2: registers at `0xCA9BC`, and what the continuation and the two entries read

The shared call site is reached from two paths of the run method `0xCA90C` (vtable `0x37CEF0` slot `+0x20`, checked: slot `+0x20` holds descriptor `0x33A268` whose code is `0xCA90C`):

- state 1 path (`0xCA9E4`..`0xCAA34`, the only one a Bluetooth object takes): `r3` = `lbz` of the handle byte `obj+0x94` (at `0xCA9B8`), `r4` = 3, `r5` = `0xF4`, `r6` = `obj+0x54` (`addi r31,r30,0x54`), `r7` = the packer's return value (`0xCBE5C` returns 4 for a 0x40 buffer; `clrldi r7,r3,0x30`), `r8` = the cookie (the run method's second argument), `r9` = `r30` = the object. `r10` is not set here.
- state 0 path (`0xCA97C`..`0xCA9B4`): the same call with `r4` = 2, `r5` = 1 (a zero output report built by `0xCC054`). No Bluetooth object ever has state 0: the only creator of vtable `0x37CEF0` objects is `0x2A33F4` (the single `ld ...,0x2998(r2)` in the image), which stores state 1 at `0x2A3420`; the USB creator at `0x2A3314` uses the other vtable `0x37CF38`, whose slot does not reference `0xCA90C`. The hook must therefore test `r4` == 3 and `r5` == `0xF4` (and the kind test is then redundant: a kind other than 3 returns `0x8001000C` at `0xCA9E4`..`0xCAA08` before reaching the site).

Read-before-write after the call, `0xCA9C0`..`0xCA9E0`: `ld r0,0xc0(r1)` (write), `ld r29`/`r30`/`r31` (restores), `mr r11,r3` (write r11 from r3), `mtlr r0`, `extsw r3,r11`, `blr`. Reads `r3` (the result), `r1`, and the callee-saved `r29`-`r31` only. It reads none of r0, r11, r12, CR, CTR before writing them, and no volatile register other than `r3`.

Entry `0x102750`: prologue `stdu r1,-0xD0(r1)`, `mflr r0`, then it reads `r3`-`r9` (copied to `r29`, `r25`, `r31`, `r28`, `r30`, `r27`, `r26`) and `r2` (TOC loads at `0x10275C`, `0x102774`) and `r1`. `r0` is written (`mflr`) before use; `r10`, `r11`, `r12`, CR and CTR are not read before the first call. The first CR use is `cmpdi cr7` at `0x1027D4`, after two calls.

Entry `0x1174FC`: prologue `stdu r1,-0x90(r1)`, `mflr r0`, then it reads `r3` (`mr r30,r3`) and `r4` (`mr r28,r4`), `r1`, and calls `0x112EC8` (HCI lock) first. Before that call it does not read `r0` (written by `mflr`), `r11`, `r12`, CR or CTR. First CR use is `cmpwi cr7` at `0x117528`, after the call.

Consequence: a branch-and-link stub entered at `0xCA9BC` may clobber `r0`, `r11`, `r12`, CTR, CR0 and CR7 (and `r10`) freely. What it must preserve: `r1`, `r2` (the kernel TOC; `0x102750` loads through `r2`), `r3`, `r4`, and `r8`/`r9` as they came in (unless deliberately changed: `r5`, `r6`, `r7` are the only ones the design changes), and every callee-saved register. LR must equal the stub's caller return address (`0xCA9C0`, set by the `bl` itself) at the moment of the tail branch to `0x102750`; the same holds for entry `0x1174FC` (tail `b` with LR intact). The `bl 0x102750` it replaces also sets LR to `0xCA9C0`, so a `bl stub` followed by `b 0x102750` from the stub (LR restored if the stub saved it) returns to `0xCA9C0` as before.

`0x1174FC` takes `r3` as a **value**: the 48-bit Bluetooth address packed in the low six bytes of the register (the three callers build it with `lbz` + `sldi`/`or` from byte offsets 6, 7, 4, 1, 2, 3 of an 8-byte address struct: `0xF4D44`..`0xF4D8C`, `0xF71E4`..`0xF7228`, `0xFDB04`..`0xFDB40`), and `r4` is the flag (1 at `0xF4D94` and `0xFDB44`; at `0xF722C` it is the caller's flag XOR 1, `xori r4,r4,1` at `0xF71E8`). It is passed on unchanged (`mr r4,r30`) to `0x1125F4` and `mr r5,r30` to the HCI formatter `0x112BE0`; it is never dereferenced.

Verdict G2: **CONFIRMED**: nothing relevant is read before it is written; the stub may clobber `r0`, `r10`, `r11`, `r12`, CTR, CR0, CR7; `r2` must be intact at the tail branch. `0x1174FC(r3 = value, r4 = flag)`.

## G3: width and signedness of `obj+0x38` (kind) and `obj+0x48` (pad)

- Kind, `obj+0x38`: `lwz r0,0x38(r30)` at `0xCA9E4` (32-bit, zero-extended; then `cmpwi cr7,r0,3` at `0xCA9F0`, a 32-bit signed compare, which is safe because kinds are 3 or 6). The store is `stw r26,0x38(r3)` at `0x2A3408` (`stw`, matching). The framework reads it as `lwz r28,0x38(r30)` at `0xCB6AC`. The hook reads it as an unsigned 32-bit word.
- Pad, `obj+0x48`: **not loaded at all inside `0xCA90C`** (the function reads only `obj+0x98` (`lbz`), `obj+0x94` (`lbz`), `obj+0x38` (`lwz`) and forms `obj+0x54`). The slot is a 64-bit pointer: stored with `std r31,0x48(r3)` at `0x2A3410` (BT object) and `0x2A332C` (USB object); read by the framework as `ld r24,0x48(r30)` at `0xCB6B8`. The hook must read it as a 64-bit `ld`. Reading `obj+0x38` and `obj+0x48` through `r9` (= the object) is safe at the call site: the object is live for the whole run method.
- Other fields of the object seen: handle byte `obj+0x94` `lbz` (stored `stb`), pad firmware word `obj+0x96` `lhz`/`sth`, state `obj+0x98` `lbz`/`stb`, retry budget `obj+0x3C` `lwz`, status `obj+0x50` `lwz`, callback `obj+0x40` `ld`.

Verdict G3: **CONFIRMED**, with one wording correction to the brief: `obj+0x38` is `lwz` (unsigned 32-bit) in `0xCA90C`; `obj+0x48` is a 64-bit pointer (`std`/`ld`) that `0xCA90C` never loads (width taken from the stores at `0x2A3410`, `0x2A332C` and the load at `0xCB6B8`).

## G4: retries rebuild the call; the payload buffer is not read after the send

Retry path (all [code]):

1. A completion event (event 2 in the worker loop `0xCBD3C`) calls `0xCB5C0`, which calls the object's vtable slot `+0x30` = `0xCB114` (checked: descriptor `0x33A2C8`, code `0xCB114`). The worker itself posts event 2 with the run method's result when that result is non-zero (`0xCBDAC`..`0xCBDBC`); the handshake reply reaches the same handler (that delivery path from the HID client was not traced here; see the start-up note C3.1).
2. `0xCB114` stores the status at `obj+0x50`. A non-zero status is returned unchanged (state `obj+0x98` stays 1); status 0 moves the state to 2 (done) for a Bluetooth object.
3. In `0xCB5C0`: a non-zero result with the budget `obj+0x3C` non-zero decrements it, waits `0x4E20` (20000, microseconds) and re-queues the object with `0xCB3BC(mgr, 1, obj, 0)` (`bl` at `0xCB748`); a zero budget goes to the callback. `0xCB3BC` posts event 1; the worker handles event 1 with `0xCB2D8`, whose virtual call is `ld r9,0x20(r11)` ... `bctrl` (`0xCB378`..`0xCB388`) = the run method `0xCA90C` again.
4. The run method `0xCA90C` re-enters the state 1 path, rebuilds the payload with `0xCBE5C` and reaches `0xCA9BC` again, so a rewritten call is rebuilt on every retry (the hook runs again each time). A non-zero return of the run method itself (for example the sender's busy status `0x80010001`, `0xF8BF8`..`0xF8C3C`) is posted as a completion with that status and follows the same retry path.

Payload buffer `obj+0x54`: only `0xCA90C` forms its address (`addi r31,r30,0x54` at `0xCA928` and `0xCA9E8`). A scan of the object's methods (vtable `0x37CEF0` slots: `0xCB2A8`, `0xCB290`, `0xC9F58`, `0xC9F60`, `0xCA90C`, `0xC9F44`, `0xCB114`, `0xCB29C`) and of the framework (`0xCB2D8`..`0xCB3BC`, `0xCB5C0`..`0xCBD3C`) found no load or store with a displacement in `0x54`..`0x93` off any register other than `r1`/`r2`. After the send, `0xCA90C` only restores registers. The final callback (`0x2A23F8` -> `0x2A2280`) receives the kind, status and pad (copied out of the object at `0xCB6AC`..`0xCB6BC`), not the object. Also, a shim-owned buffer (the design's `po_buf`) is not touched by the kernel after the call returns; see G5.

Verdict G4: **CONFIRMED**.

## G5: sender copies the payload; the packet fits

`0xF8BD8(client r3, type r4, id r5, payload r6, length r7, cookie r8, context r9)`:

- Busy check first: `lbz r0,0x167(r27)`; non-zero returns `0x80010001` (`0xF8C2C`) without touching anything.
- Otherwise it loads the global packet buffer address (`ld r29,-0x3370(r2)` = `0x4326C8`), clears the first `0x41` bytes (`li r5,0x41`, call `0x51014`), stores the header byte (`ori r0,r31,0x50`, so type 3 gives `0x53`) at `buf+0` and the report id at `buf+1` (`0xF8C7C`..`0xF8C90`), and copies the payload with `0x82A08(buf+2, payload, length16)` at `0xF8C94`. The copy helper is a leaf (byte loop, or a wide copy for larger sizes) that only stores into its destination; it keeps nothing.
- It then calls `0x116EF0(addr, 2 (control channel), 2 (mode), buf, length+2)` at `0xF8D04`, and on success sets the busy byte `client+0x167` and stores **only** the cookie (`std r24,0x178(r27)`) and the context (`std r25,0x180(r27)`), plus zero at `client+0x168` (`0xF8D20`..`0xF8D38`). The payload pointer `r26` is never stored anywhere. The copy is complete before `0xF8BD8` returns.
- No cap on the length (16-bit): the interrupt sender `0xF8D60` has the `cmplwi 0x40` cap at `0xF8D74`, `0xF8BD8` does not.

Fit of a 48-byte wire packet (header `0x53`, id `0x08`, 46-byte payload; `length + 2 = 48` is passed down):

- Packet buffer: `0x4326C8`; the next separately referenced global is `0x432710` (+0x48 = 72 bytes; `0x4326C9` is a byte alias of `buf+1`). The shared sender `0xF8D60` already writes up to 66 bytes there. 48 < 72, and 48 < the 65 bytes the sender clears first.
- Control-channel transmit ring: channel 2 is opened with capacity `0x43` = 67 (`li r7,0x43` at `0xF9424`, channel 2 at `0xF9414`; channel 3 the same at `0xF9494`; channels 0 and 1 use `0x86` = 134). `0x1169E4` for channels 0 and 2 requires `capacity - used >= length + 2` (`0x116AFC`..`0x116B18`), so the record is `48 + 2 = 50` bytes and needs `used` <= 17. [inferred] the ring is empty at that moment (the only other control-channel traffic is the 6-byte `F4` enable and small GET_REPORT requests, and the busy byte `client+0x167` serialises SET_REPORTs until the handshake); not provable statically.
- Nothing below compares against an MTU (see the output facts note F9.2/F2 for the HCI side); the HCI ring is 2048 bytes.

Verdict G5: **CONFIRMED** (copy-before-return, no pointer kept; 48 fits 72 and fits the 67-byte ring with its 2-byte prefix when the ring is drained, the one inferred point).

## G6: callers of `0x1174FC`

Direct callers (index re-run on both images): `bl` at `0xF4D94`, `b` at `0xF722C`, `bl` at `0xFDB44`, and `bl` at `0x117728` (inside the wrapper `0x117704`, which sets the flag to 1). No other `b`/`bl` and no data word holds the entry or its descriptor (`0x342AA0`, zero references), so there is no indirect call.

- `0xF4D94`: inside the BT control syscall `0xF4B68` (descriptor `0x33EF48` referenced at `0x38B8C0` = entry 662 of the syscall table at `0x38A410`), command `0x16` (jump table `0xF4C18` entry `0x16` = `0xF4CF0`): copy 8 bytes from the user, delete the stored-pairing entry (`0xF6174`), then `0x1174FC(addr, 1)`. User-reachable (a syscall). Known to the spec.
- `0xF722C`: tail `b` in `0xF71E4` (no descriptor, one direct caller `0x102744` inside `0x10268C`). `0x10268C` (descriptor `0x3408B0`, zero data references) has five direct callers: `0xE1424`, `0xE1A44`, `0xE3C54` (HID-core / gamepad class; `0xE3C54` is the gamepad ioctl command 6 reached from syscall entry 648) and `0x2AABD8`, `0x2AB4CC` (second driver class, the BD remote). No pad-class (`0x2A0000`..`0x2A5000`, `0xC9000`..`0xCC000`) caller. Known to the spec ("reached through `0x10268C`") and to the facts note P3.
- `0xFDB44`: inside `0xFDAB8`, the HID client vtable `0x37DFA0` slot `+0x68` (descriptor `0x33FF50` referenced only at `0x37E008` = `0x37DFA0 + 0x68`). `0xFDAB8` has no direct caller. A search for virtual calls through a slot `+0x68` finds 89 sites in the whole image and none in the HID/Bluetooth code (`0xE0000`..`0x120000`) or in the pad class; the receivers of the 89 sites cannot be typed statically, so who invokes this slot is [inferred] to be a generic path (not found). It is a user-triggered path at most through something that is not a direct caller.
- `0x117728`: the wrapper `0x117704` has no direct caller and one data reference: its descriptor `0x342AB8` is held in the TOC slot `-0x25E8(r2)` (`0x3733D8`), which is loaded only once, at `0x11769C` **inside `0x1174FC` itself**, and passed to the deferred-callback registrar `0x11BE88` (`0x1176B4`) with the address as argument. So `0x117728` is a timer re-entry that `0x1174FC` schedules for itself (connection state 6 and a recent timestamp, `0x11767C`..`0x1176B8`), after one of the three user paths has already called it. It is not a fourth independent request. It is not user-reachable on its own, and it is not covered by the three call-site observers; an observer at the entry `0x1174FC` would see it, one at the call sites would not (by design).

Verdict G6: **CONFIRMED**: the user-reachable callers are `0xF4D94`, `0xF71E4` (via `0x10268C`) and `0xFDB44`, as the spec says; one additional direct site (`0x117728`) exists but is only a self-scheduled timer re-entry of `0x1174FC`. The only point not provable statically is who calls the HID client vtable slot `+0x68`.

## Verdicts

| Item | Verdict | Support |
|---|---|---|
| G0 | CONFIRMED (controller; third-party source only) | `dualsensectl` `main.c` / `crc32.h` |
| G1 | CONFIRMED | four decodes, second half of each qword, four hashes equal in both images |
| G2 | CONFIRMED | nothing relevant read before written after `0xCA9BC`, at `0x102750` and at `0x1174FC`; keep `r2`, LR; `r3` of `0x1174FC` is a value |
| G3 | CONFIRMED (wording) | kind `lwz` at `0xCA9E4`; pad is `ld`/`std` (`0xCB6B8`, `0x2A3410`), not loaded in `0xCA90C` |
| G4 | CONFIRMED | `0xCB5C0` -> `0xCB3BC` -> `0xCB2D8` -> slot `+0x20`; `obj+0x54` read by nothing after the send |
| G5 | CONFIRMED | copy at `0xF8C94`, only cookie/context kept at `0xF8D2C`/`0xF8D30`; buffer 72 (`0x4326C8`), ring 67 (`0xF9424`) |
| G6 | CONFIRMED | three user-reachable sites as in the spec; fourth site `0x117728` is a self-scheduled timer re-entry |

No mismatch against the spec. Remarks for the later tasks (none change a value): the shared call site is also reached with `r4` = 2, `r5` = 1 by the (unused for Bluetooth) state 0 path, so the `r4` == 3 / `r5` == `0xF4` test is required; the hook reads the kind as a 32-bit word and the pad as a 64-bit pointer from the object in `r9`; the state/handler of the command object means the hook will run again on every retry.
