# DS45Pad native DS3 route, spec 1: input and USB pairing

Date: 2026-10-30. Status: design approved in conversation; awaiting review of
this written spec.

## Goal

A DualSense connects over Bluetooth and runs through the PS3's own Sony-pad
(DS3) driver, like a DS3: it is paired by plugging it in by USB once, then
takes the next free port with one port per pad, up to seven pads, with no
prompts, menus or toasts. The PS3 assigns the player number itself.

Spec 1 covers input only. Spec 2 (later) covers output: rumble, player LEDs,
light bar, adaptive triggers. DS4 support is a later add-on; a DS4 keeps
today's behaviour meanwhile.

## Constraints (from the user)

- Works like a DS3 or better; never asks the user anything. Verification is
  passive only.
- Target: CFW with syscalls available. Online play with syscalls disabled is
  out of scope beyond graceful behaviour. Once installed, the shim's driver
  redirects stay until reboot even if syscalls are disabled later (user
  decision); no extra syscall stays behind.
- All kernel writes are RAM-only, made only on fingerprinted kernels (table
  entries; DEX 4.92 first), verified against the expected original before
  writing and read back after.
- Nothing Sony-proprietary in git: no SDK files, keys, dumps, kernel code
  bytes or SDK-linked binaries. Expected original kernel code is stored as a
  hash, like the existing fingerprints; expected original pointers (kernel
  addresses) may be stored as values. The compiled shim blob is a build
  output and is never committed.
- No degraded mode: if anything required is missing, the shim is not
  installed and every pad keeps today's behaviour.

## What is known (DEX 4.92, from on-console work)

- The link-key table entry's type byte selects the driver: 0x01 Sony, 0x08
  generic. With type 0x01 and the Sony product-ID checks accepting 0x0CE6
  (sites 0x11B5D8 and helper 0x29E2B4), the PS3 creates a Sony pad object
  (class vtable 0x38CDC8) for a DualSense, but it never reaches running and
  gets no port.
- The Sony pad's handler 0x2A4E40 pulls input itself: transport object at
  pad+0x4F0 (class 0x37CE18), method 4 (0x0C9D60) drains the connection's
  queued input into the shared buffer 0x4326C8 (0x41 bytes), then requires
  report ID 0x01 and the 49-byte DS3 layout, which converter 0x2A4678 maps
  into CellPadData.
- The DS3 start-up step (per-transport descriptor tables 0x37CF20, 0x37CF68,
  0x37CFB0, slot 7: 0x0CAA3C / 0x0CAB84 / 0x0CACF0) sends HID SET_REPORT
  feature 0xF4 ("enable reporting") through 0x102750 (Bluetooth) or 0x107C18.
- In-plugin capture during a DualSense connect: the only message ever placed
  in 0x4326C8 starts with 0x02, then silence for six minutes. Most likely a
  Bluetooth HID handshake "invalid report ID": the DualSense rejects 0xF4 and
  the driver never starts polling.

## Design

### Components

1. `shim/` (new): freestanding PPC64 kernel code, built with
   `ppu-lv2-gcc -ffreestanding -nostdlib` into flat position-independent
   code and embedded in the plugin at build time.
   - Start-up wrapper and input wrapper. Each decides "is this connection a
     DualSense (VID 054C, PID 0CE6)?" and otherwise tail-calls the original,
     so a DS3 (or anything else) is untouched.
   - Product-ID acceptance: 0x0CE6 is accepted in addition to the IDs the
     kernel already accepts (DS3 0x0268, Move navigation 0x042F), so neither
     loses the Sony path. Mechanism decided in plan task 1: a redirect of the
     helper and the inline check to a blob routine (preferred), or replacing
     the 0x042F immediates (drops Move navigation over Bluetooth; only with
     the user's agreement).
   - Translator `ds3_report.c`: DualSense Bluetooth report 0x31 to the
     49-byte DS3 report 0x01. Reuses `dualsense_parse()`, split so it builds
     without SDK headers.
   - Shared data block: magic, version, enable flag, per-connection state,
     and the config values the shim honours (stick deadzone, MAP_TOUCHPAD,
     MAP_MUTE, ACCEL_SCALE, GYRO_SCALE). Written by the plugin at install.
2. `src/kshim.c` (new): installer in the plugin (see Install).
3. `src/kernel.c`: each firmware entry gains the shim fields: allocator,
   syscall table, spare syscall slot, the redirect slots and their expected
   original pointers, the product-ID sites and their expected-code hashes.
4. `src/usbpair.c` (new): DS3-style USB pairing (see Pairing).

The USB path and the generic Bluetooth path are unchanged and remain what
happens whenever the shim is not installed.

### Start-up

A DualSense registered as a Sony pad connects; the driver creates its pad
object and runs the start-up step. For a DualSense the start-up wrapper does
the DualSense's own enable instead of 0xF4: a GET_REPORT of feature 0x05 over
Bluetooth through the driver's own send helper, which switches the pad to
full reports (0x31, with motion). It then gives the driver's state machine
what it needs to continue as after a DS3 acknowledgement, so the pad object
reaches running and gets the next free port.

### Input

On every poll the input wrapper calls the original transport read (driver
locking and queue draining unchanged). If the connection is a DualSense and
the result is report 0x31, it translates it in place into DS3 report 0x01 and
returns length 49; the handler's checks and the converter then run exactly as
for a DS3. Anything else (another report ID, a short read, another device)
passes through unchanged and the driver ignores it as today.

Mapping:
- Buttons, D-pad, sticks direct; stick deadzone from config.
- Create = Select, Options = Start, PS = PS. Touchpad click and Mute follow
  MAP_TOUCHPAD / MAP_MUTE (defaults as today).
- Pressure: L2/R2 from the analog triggers; other pressure bytes 0 or 255.
- Motion: DualSense accelerometer and gyro rescaled to the DS3's 10-bit,
  512-centred sensors with today's ACCEL_SCALE / GYRO_SCALE math.
- The report CRC is not checked on this path (the 0x41-byte buffer holds
  everything used but not the CRC).

### Pairing (USB cable, like a DS3)

When a DualSense is plugged in by USB (the plugin already detects it):
1. Read feature 0x09: the pad's Bluetooth address and current host.
2. Get the PS3's own Bluetooth address.
3. Already paired to this PS3 and registered as a Sony pad with a key: stop,
   nothing written.
4. Otherwise generate a 16-byte link key from the system random source,
   write host address and key with feature 0x0A, read back with 0x09.
5. Register the pad with the PS3 under its address as type Sony with that
   key, through the system's own registration path so it persists like any
   paired device. An existing entry for that address (e.g. a menu-registered
   generic one) is replaced.
6. While plugged in it works as a wired pad through today's USB path; once
   unplugged, PS connects it natively over Bluetooth.

No prompts or toasts; the debug build traces each step.

Decision rule: if no system registration call accepts a Sony type with a key,
register through it as generic and have the plugin set the entry's type to
Sony in RAM when the pad is seen (the proven type flip, verified and read
back), on every boot. Same result for the user. Reported at plan time.

### Install

Only on a kernel matching a table entry that has shim fields, with every
target verified first. Fixed order:
1. Point the spare syscall slot at the LV2 allocator, call it once, restore
   the slot, read back.
2. Copy the blob and the data block; read back.
3. Make the instruction cache coherent for the blob (plan task 1 settles
   whether the CFW poke does this or an LV2 flush routine is used).
4. Product-ID acceptance, then the start-up and input redirects: each an
   aligned 8-byte pointer write (atomic on the PPU), read back.

Any failure undoes every write already made, in reverse order; pads keep
today's behaviour; the debug trace names the step. On plugin unload the
redirects are switched back; the blob stays allocated, since a driver thread
may still be inside it.

### Shim code rules

Never blocks or allocates, no unbounded loops, touches only DualSense
connections, bounds-checked against the actual read length, needs nothing
from the plugin process after install (works in games and after a plugin
crash).

## Facts to settle read-only before coding (plan task 1)

Each with its decision rule; anything unsatisfiable means no install.
1. What moves a pad object from start-up to running: the send's return value
   or a reply from the pad. The start-up wrapper provides it.
2. Whether the input queue keeps a DualSense report longer than 0x41 bytes
   (everything used is in the first 65).
3. How a GET_REPORT of feature 0x05 is issued with the driver's helper.
4. Product-ID acceptance mechanism (see Components).
5. Instruction-cache coherency for freshly written kernel code.
6. The LV2 allocator, syscall table and a spare slot on DEX 4.92.
7. Pairing: where the PS3 keeps its own address; which registration call
   persists an entry; whether it accepts Sony type plus a key.

## Testing

Host (native clang, existing harness):
- Translator golden tests from recorded DualSense reports; fuzz with random
  bytes at every length (no out-of-bounds, non-0x31 passes through).
- Installer against a fake LV2 that records writes: order, every rollback
  branch, nothing written on a fingerprint or expected-original mismatch.
- Wrapper decision logic (C) and the pairing state machine (fake USB
  feature reports, fake registration).

Console (scripted, RAM-only, one stage at a time, DEX 4.92):
1. DS3 over Bluetooth and USB still works with the shim installed.
2. DualSense with its type flipped by debug tooling gets a native port;
   input matches.
3. USB pairing; reboot; still paired and native.
4. DS3 plus two DualSenses get ports 1-3.
5. Plugin unload switches the redirects back.

## Out of scope

Output (spec 2), DS4 and DualSense Edge, firmwares other than table entries,
online play with syscalls disabled.
