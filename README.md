# NativePadPS3 — modern controllers on the PlayStation 3

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Platform](https://img.shields.io/badge/Platform-PS3_CFW_%7C_PS3HEN-orange.svg)]()
[![Status](https://img.shields.io/badge/Status-work_in_progress-yellow.svg)]()

A background VSH plugin for the **PlayStation 3** that brings modern Sony controllers — the **PS5 DualSense** (`054c:0ce6`), **DualSense Edge** (`054c:0df2`), and **DualShock 4** v1/v2 (`054c:05c4`, `054c:09cc`) — to the console over **USB and the PS3's internal Bluetooth radio**, including the physical **PS / Home button** with no button combinations.

> This is an **independent continuation** of the original DualSense-PS3 / DS45Pad work. It is not affiliated with or endorsed by Sony or the original authors. See **[CREDITS.md](CREDITS.md)** for the upstream projects it builds on and why this repository starts from a fresh history.

---

## ⚠️ Status and disclaimer

This is **work-in-progress homebrew that patches a running console**. Use it only if you understand PS3 custom firmware and can recover a console that fails to boot (see **Recovery & Failsafe** below). It is provided with no warranty of any kind; **you are responsible for what you run on your own hardware.**

The project currently has two paths, at very different levels of maturity:

| Path | What it does | Maturity |
|---|---|---|
| **Passthrough path** (inherited) | Presents the controller through a virtual pad; uses Port 7 + Syscall 573 to avoid double input. | Inherited from upstream. Developed and reported working by the original author on PS3 Super Slim / PS3HEN 4.93; **not independently re-verified in this repository.** |
| **Native DS3 route** (new) | Runs a DualSense through the PS3's own DualShock 3 driver so it gets a real native port — no Port 7, no input injection. | **Early, experimental.** Validated on a single **DEX 4.92 (Evilnat CFW)** development console. **Off by default**, debug builds only, RAM-only. Known limitations below. |

Nothing in this repository has been verified across firmwares or console models beyond what is stated above. Treat untested combinations as unknown.

---

## 🎮 What it does

* **Native internal Bluetooth** — connects to the PS3's built-in radio; no external dongle.
* **USB and Bluetooth** — `MODE = AUTO` uses USB when cabled and Bluetooth otherwise.
* **Physical PS / Home button** — no Select+Start style combinations.
* **Controller support** — DualSense (`054c:0ce6`), DualSense Edge (`054c:0df2`), DualShock 4 v1/v2 (`054c:05c4`, `054c:09cc`); more can be declared in `ds45pad_devices.txt`.
* **Build from source** — no binaries are distributed here (see below).

---

## 🧪 The native DS3 route (experimental)

The newer direction in this project makes a DualSense behave as a **native DualShock 3** rather than as an injected virtual pad. A RAM-only kernel shim lets the DualSense complete the PS3's own DS3 start-up handshake and run through the stock Sony-pad driver, so it is assigned a genuine native controller port and its reports are translated into DS3 reports in place. Because the controller is native, the Port 7 reassignment and Syscall 573 injection that the passthrough path needs are not used in this mode, and a real DS3 plugged in alongside stays fully functional.

**What has been observed working** (on the one DEX 4.92 console it was developed against): a DualSense completing DS3 start-up and getting a native port over both Bluetooth and USB, across connect and disconnect, with two DualSense pads usable on separate ports at once, buttons and sticks translated, and no system freeze.

**Known limitations (open work):**
* The battery indicator follows the pad's own status byte (see the output section below); the charging and full indications have not been verified on hardware.
* **"Turn Off the controller"** from the PS button menu disconnects the pad but does not power it off (no power-off path yet).
* No touchpad and no USB-cable pairing yet.
* The Move navigation controller is accepted again but unproven on hardware (no navigation controller was available); the Move motion controller is untouched.

**Output: rumble, player LEDs and light bar.** With the shim on, each DualSense gets what the PS3 asks of a DualShock 3, translated in the kernel's Bluetooth output path. What you see on a DualSense:
* **Rumble** from games, which stops when the game stops it.
* **Player LEDs** that show the controller number (two dots for port 2, three for port 3, and so on).
* **Light bar colour by controller number**: port 2 red, port 3 green; the table also holds blue for port 1 and pink for port 4. A real DS3 on another port is untouched.
* A DualSense that is powered off by force reconnects with the same controller number. This was verified on two pads across runs (DualSense #2 in the identity run, #3 in the output run) and relies on `SHIM_STABLE_ID` (default `1`, the pad's Bluetooth address), with the per-boot counter as the fallback.

It is controlled by two VSH-only keys (see the table below); like the rest of the shim it needs `ENABLE_SHIM = 1`, exists only in debug VSH builds, is RAM-only and is DEX 4.92-only. Known limits:
* Only ports 2 (red, two LEDs) and 3 (green, three LEDs) were seen in full mode on the console. The colours and LED patterns for ports 1 and 4 come from the same table but were not looked at. The colours are chosen, fixed constants (blue, red, green, pink for ports 1 to 4), not configurable.
* Rumble was seen from a game only (a split-screen game's effect reached both the DS3 and a DualSense); other rumble sources were not separately tried.
* The battery level is read from the pad's own status byte and shown as the pad reports it; the charging and full values of the DS3 report have not been seen on hardware.
* The newer-firmware rumble flag (`valid_flag2` bit 2) is untried, because the tested pads rumble with the current flags.
* Verified on a DEX 4.92 console with one DS3 and two DualSense pads; a Move navigation controller and other long-report devices were not exercised.
* The rewriting stages (modes 1 and 2) need the input hook (`SHIM_INPUT_HOOK`, default `1`): the DualSense switches to a longer input report that only the input hook can take in. With `SHIM_INPUT_HOOK = 0` the output hook only observes, whatever `SHIM_OUTPUT_MODE` says.
* Stopping or unloading the plugin leaves a DualSense that is already in the longer (full) report mode without input until it reconnects: the pad cannot be switched back from the console side.

**Safety gate.** The shim patches live kernel code, so it is never installed on its own. It is enabled **only** when `ds45pad.txt` contains `ENABLE_SHIM = 1`, and it is built only into debug VSH builds. The way out of a hang is to remove that line (or the plugin) and reboot. Leave `ENABLE_SHIM` unset for normal use.

---

## ⚙️ Configuration (`ds45pad.txt`)

Edit `/dev_hdd0/plugins/ds45pad/ds45pad.txt`. Common options:

| Option | Default | Description |
|---|---|---|
| `ENABLE` | `1` | `1` = plugin on, `0` = off. |
| `MODE` | `AUTO` | Transport: `AUTO` (USB when cabled, else Bluetooth), `USB`, or `BT`. |
| `MAX_PADS` | `1` | Number of controllers handled at once. |
| `POLL_MS` | `10` | Input polling period in ms (10 ms ≈ 100 Hz). |
| `INSERT_INTO_GAME` | `1` | Passthrough path: inject pad input into games via Syscall 573. |
| `ALLOW_INSERT_COMBO` | `1` | Passthrough path: toggle in-game injection with `CREATE + OPTIONS + R3`. |
| `STICK_CALIBRATION` | `OFF` | Stick response-curve calibration. |
| `NOTIFY` | `1` | Show an on-screen notification on load/attach. |
| `DIAG` | `0` | Diagnostic file logging; leave off for best performance. |
| `ENABLE_SHIM` | `0` | **Experimental.** Install the native DS3-route kernel shim (debug builds only). Leave unset unless you are testing that route. |
| `SHIM_OUTPUT_HOOK` | `1` | Shim only (needs `ENABLE_SHIM = 1`; debug VSH builds). Install the output hook that gives a DualSense rumble, player LEDs and a light bar colour. `0` = off. The rewriting stages need the input hook (`SHIM_INPUT_HOOK`, default `1`); with `SHIM_INPUT_HOOK = 0` the output hook only observes. |
| `SHIM_OUTPUT_MODE` | `2` | Shim only. What the output hook does: `0` observe (hook installed, nothing changes), `1` probe (fixed blue light bar and player LED 1, for tests), `2` full (rumble, player LEDs and light bar colour by controller number). Modes 1 and 2 only take effect with the input hook on (`SHIM_INPUT_HOOK`, default `1`); without it the hook stays an observer. |

The configuration parser accepts a number of additional debug and shim-tuning keys used during development of the DS3 route; they default to off/safe and are not needed for normal use.

---

## 🎮 Adding third-party / clone controllers

Edit `/dev_hdd0/plugins/ds45pad/ds45pad_devices.txt`:

```text
# Format: VID, PID, MODEL   (MODEL = DUALSENSE or DS4)
0x054c, 0x0ce6, DUALSENSE
0x054c, 0x0df2, DUALSENSE
0x054c, 0x05c4, DS4
0x054c, 0x09cc, DS4
```

---

## 🚀 Installation (passthrough path)

This repository contains **no binaries** — build the plugin first (below). The [`ps3_ready/`](ps3_ready/) folder holds only the configuration `.txt` files.

1. **Copy files** to `/dev_hdd0/plugins/ds45pad/` (via FTP through multiMAN / webMAN MOD, or a FAT32 USB drive):
   * your build output `ds45pad_vsh.sprx`
   * `ps3_ready/plugins/ds45pad/ds45pad.txt`
   * `ps3_ready/plugins/ds45pad/ds45pad_devices.txt`
2. **Autoload:** add this line to `/dev_hdd0/boot_plugins.txt` (or `boot_plugins_nocobra.txt`), ending with a blank newline:
   ```text
   /dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx
   ```
3. **Pair over Bluetooth** (first time): **Settings → Accessory Settings → Manage Bluetooth Devices → Register New Device**, then hold **Create (Share) + PS** on the DualSense until the light bar blinks rapidly, and select **Wireless Controller** (passkey `0000` if asked).
4. **Reassign to Port 7** (passthrough path only): **Settings → Accessory Settings → Reassign Controllers**, change the controller to **7**. This keeps the dashboard from seeing double input while Syscall 573 maps the pad to Player 1 in games. *(The native DS3 route does not need this step.)*
5. **Reboot**, enable HEN if on PS3HEN, and press **PS** to connect.

---

## 🛡️ Recovery & failsafe

If the console fails to boot or freezes:
1. **USB failsafe** — put an empty file named `boot_plugins.txt` in the root of a FAT32 USB drive (`/dev_usb000/boot_plugins.txt`); CFW/HEN prioritizes it and skips the HDD plugins.
2. **FTP recovery** — boot safe mode or multiMAN, connect over FTP, and rename or delete `/dev_hdd0/boot_plugins.txt`.
3. If you were testing the DS3 route, removing the `ENABLE_SHIM = 1` line (or the plugin) and rebooting returns the console to stock behavior.

---

## 🛠️ Building from source

### Prerequisites
* Sony Cell SDK PPU toolchain (`ppu-lv2-gcc`)
* `scetool` with your own retail keys
* Windows (PowerShell) or Linux (Bash + Wine or a native toolchain)
* The native DS3-route shim additionally needs a freestanding `clang` + `ld.lld` to build its position-independent blob (VSH debug variants only).

> **Legal notice:** proprietary Cell SDK headers, libraries, and encryption keys are **not** included. You must supply your own SDK and keys to compile.

### Build (Windows / PowerShell)
```powershell
cd src
.\build.ps1 -Variant vsh
```

The signed plugin is written to `src/ds45pad_vsh.sprx`. The `game` variant builds without the DS3-route shim; only the VSH variants build it.

---

## 🚫 Why there are no binaries in this repository

* **SDK stub code.** A built `.sprx` links code from the proprietary Sony Cell SDK, which cannot be redistributed.
* **Retail keys.** A retail-signed `.sprx` is signed with leaked Sony keys, which also must not be redistributed.
* **Build output only.** Binaries are build output, not source — build them locally.

To keep the tree clean, `tools/check_no_proprietary.ps1` rejects banned file types (`.sprx`, `.prx`, `.self`, `.elf`, `.a`, `.bin`, `.pup`, …), key/dump paths, SDK copyright markers, and unlisted binary files. It runs as a git pre-commit hook (`git config core.hooksPath .githooks`) and can be run manually over the whole tree.

---

## 📜 Credits

Built on the work of the PS3 homebrew community (full details in **[CREDITS.md](CREDITS.md)**):

* **[Dobridp](https://github.com/Dobridp)** — original [DS45Pad](https://github.com/Dobridp/DS45Pad).
* **domyno6773 / DualSense-PS3** — the DualSense-PS3 continuation this project forked from.
* **[OsirisX](https://github.com/OsirisX)** — [PS3xPAD](https://github.com/OsirisX/PS3xPAD), the basis for custom-controller LDD injection on PS3.
* **[aldostools](https://github.com/aldostools)** — webMAN MOD, PRX signing guides, and PS3 VSH documentation.
* **PS3Xploit Team** — PS3HEN and LV2 syscall research.

---

## ⚖️ License

Free and open-source software under the **GNU General Public License v2 (GPL-2.0)**. See [LICENSE](LICENSE).
