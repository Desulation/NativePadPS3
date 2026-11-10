# What already existed

A survey done before writing a single line, so as not to start from scratch.
Summary: there is **one** serious project in this area, it has been idle since
2015, and its core technique is sound and was reused here.

---

## PS3xPAD (OsirisX, 2015)

Repository: <https://github.com/aldostools/PS3xPAD> · forum:
<https://www.psx-place.com/resources/ps3xpad.22/>

The only PS3 controller plugin with open source code. Latest version 0.8, from
December 2015. It supports Xbox 360/One, DS3 and DS4. It **does not support
DualSense**; the controller didn't even exist.

### What it got right, and this project inherits

Three discoveries that solve exactly the pain points mentioned:

1. **`syscall 574`** registers an LDD pad with an *arbitrary capability mask*.
   That is what makes it possible to announce
   `CELL_PAD_CAPABILITY_PS3_CONFORMITY` and get games that require an official
   controller to accept the virtual pad.

2. **`syscall 573`** turns on *data insert mode*, without which the injected
   data only reaches the XMB and never the games.

3. **`button[0] |= CELL_PAD_CTRL_LDD_PS`**: the PS button is not a normal
   button. It lives in an extra word of the report that only exists for LDD
   pads. It is literally the reason the PS button doesn't work on a regular DS4.

The general flow (`cellUsbdRegisterExtraLdd` to take the device from the system
driver → read HID reports → `cellPadLddDataInsert`) also came from there.

### What was left behind

| PS3xPAD limitation | Here |
|---|---|
| No DualSense | Supported, including Edge |
| 10-sample ring buffer drained 1 at a time → input lag | Double buffer, always the newest sample |
| Sixaxis "4x too sensitive", fixed by guesswork | Physical conversion (8192 LSB/g → 113 LSB/g) |
| No touchpad, player LEDs, battery | Supported |
| Wireless required a USB dongle + a whole Bluetooth stack | Passthrough over the internal Bluetooth, no extra hardware |
| `capability = 0xFFFF` (undefined bits) | Explicit mask, configurable |

---

## Other alternatives evaluated

| Option | Verdict |
|---|---|
| **Brook / CronusMax / Titan adapters** | They work well, PS button included. A hardware solution: they emulate a real DS3 over USB (the PS3 has no cryptographic controller authentication, it only checks VID/PID + report format). They cost money and it is not what was asked for. |
| **The PS3's own generic driver** | It is what already happens today. It accepts DS4 over USB and over Bluetooth (pairing with PS+Share), but discards PS, motion, touchpad and vibration. |
| **LV2 kernel patch** | It would fix vibration without a game plugin, but requires a custom Cobra/Mamba payload: high risk, small gain. Discarded. |
| **PSL1GHT / ordinary homebrew** | Doesn't work: a homebrew `.self` doesn't run alongside games. It has to be an SPRX loaded by the plugin loader. |

---

## Technical references used

**HID report format**: the Linux `hid-playstation` driver
(<https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c>).
It is the best public documentation of the DS4 and DualSense: offsets, report
IDs, the `0xA2` CRC32 seed for Bluetooth reports, sensor scales
(`DS_ACC_RES_PER_G = 8192`, `DS_GYRO_RES_PER_DEG_S = 1024`) and the VID/PIDs.
It also confirmed that the DualSense Edge paddles are **not** decoded by the
kernel, which is why those bits are marked "verify" in the code.

**`cellPad` API**: the RPCS3 implementation
(`rpcs3/Emu/Io/pad_types.h`, `rpcs3/Emu/Cell/Modules/cellPad.cpp`). It is where
the exact values of `CELL_PAD_CTRL_LDD_PS = 0x0001`, `CELL_PAD_CAPABILITY_*`
and the offsets of `button[]` came from, along with the confirmation that
`cellPadSetActDirect` maps `motor[1]` to the big motor and `motor[0]` to the
small one, and that the pad **must** announce `CAPABILITY_ACTUATOR` or the
vibration call is rejected with `CELL_PAD_ERROR_UNSUPPORTED_GAMEPAD`.

**NIDs**: on the PS3 they are `SHA-1(name + Sony's 16-byte suffix)`, first 4
bytes read as little-endian. The algorithm was validated against three
already-published values before being used:

| Symbol | Calculated | Matches |
|---|---|---|
| `allocator/malloc` | `0x759E0635` | fixed value in PS3xPAD ✅ |
| `allocator/free` | `0x77A602DD` | fixed value in PS3xPAD ✅ |
| `sys_io/cellPadInit` | `0x1CF98800` | known public NID ✅ |
| `sys_io/cellPadSetActDirect` | **`0xF65544EE`** | used in the vibration hook |

**SPRX build**: the PS3xPAD and webMAN MOD Makefiles. Both use the official
Cell SDK (`ppu-lv2-gcc -mprx`) and sign with `scetool` using exactly the same
parameters, which is what makes a plugin load on both CFW and HEN.

**DS4 behavior on the PS3's Bluetooth**: consistent community reports (Push
Square, GamesRadar and others) confirm that native pairing has worked since a
system update, but without touchpad, sixaxis, PS button or vibration. It is
exactly the scenario the passthrough mode addresses.

---

# What we learned by measuring on the console

This section is the result of debugging on hardware, and is the part that was
not in any documentation. It is worth more than the survey above.

## About virtual pads

**An LDD pad always reports `device_type = 5`.** Never `STANDARD` (0). Asking
for capability `0x1F`, the system grants `0x07` (conformity, pressure and
sixaxis) and denies the two hardware bits. The type is unchangeable.
Consequence: **no plugin based on a virtual pad can satisfy a game that checks
the device type.** This holds for this project, for PS3xPAD and for any other.

**Syscall 573 (`INSERT_DATA_INTO_GAME_MODE_ON`) keeps apps from loading.** With
it on, any app started afterwards, multiMAN included, never finishes coming up.
The virtual pad itself is harmless; it is the exposure to games that hangs.
Isolated by bisection: no pad → opens; pad without 573 → opens; pad with 573 →
hangs. PS3xPAD hits the same wall, and that is why its README tells you to
press `START+SELECT+R3` **after** the game starts.

**`CELL_PAD_CTRL_LDD_PS` works, but only in the XMB.** The PS button in a game
is handled by the VSH, not by the game; no hook in the game process reaches it.

## About USB

**The firmware's generic driver does not let go of the DualSense.**
`cellUsbdRegisterExtraLdd` does not take its device: the attach callback never
fires (diagnostic `probe_NEVER_CALLED`). The controller stays on port 0 as a
generic pad, `device_type = 1`, capability `0x00`. PS3xPAD suffers from the
same problem: "0 USB controllers detected" with the controller plugged in.

**A game process does not load `libusbd`.** The module list of a running title
has `libio` and `libfs`, but not `libusbd`. A single import from it leaves the
loader unable to resolve the module, and PS3MAPI gets stuck on "Load" forever.

## About signing

**Key revision `0A` (3.55) is mandatory on HEN.** Older ones hang the boot.

**The auth-id depends on where the module will run:**

| Target | auth-id |
|---|---|
| VSH process | `1070000052000001` (sys/internal + vsh/module) |
| Game process | `1010000001000003` (retail game/update) |

PS3xPAD's `xpad_game.sprx` uses the second, for the same reason.

## About function detours

**`*(uint32_t *)0x1008C + 0x984` only resolves in the VSH process.** It is the
table walk that the published plugins use; in a game process 0x10000 is the
EBOOT's ELF and there is no table there.

**The path that works in any process:** take the pointer of the imported
function, which on PowerPC64 ELFv1 is already the address of the descriptor
`{entry point, TOC}`, and overwrite the two words. Measured: the descriptor of
`cellPadGetData` landed ~1.6 MB outside the injected module, meaning it is the
shared one inside `sys_io`, and patching it redirects the whole process.

## About the analog stick curve

The central hypothesis held up: the DualShock 3 converter saturates at ~80% of
the mechanical travel, and reproducing that with a single parameter makes the
DS4, DualSense and Hall effect deliver the same numbers as a DS3. Verified in
simulation with C's integer arithmetic, and confirmed in game. Details in
[ANALOG_STICKS.md](ANALOG_STICKS.md).
