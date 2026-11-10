# Building NativePadPS3

> **No binaries are distributed in this repository.** The `ps3_ready/` folder only
> contains the ready-to-copy `.txt` files; you build the signed `.sprx` yourself
> with the commands below (or with `src/build.ps1`). This document explains how.

## Prerequisites

Building a VSH SPRX plugin requires the **Sony Cell SDK PPU toolchain** (`ppu-lv2-gcc`). Signing uses either the SDK's own `make_fself` or `scetool`.

| Tool | Notes |
|---|---|
| Cell SDK (PPU) | `ppu-lv2-gcc` in PATH or specified via `CELL_SDK` environment variable |
| `make_fself` (SDK) | Default when `NPP_TOOLS` is unset. Produces a fake-signed SPRX that loads on **DEX/debug firmware only** |
| `scetool` | Used when `NPP_TOOLS` is set. Signs with 3.55 retail keys (`Revision 0A`) so the SPRX loads on CEX CFW and PS3HEN |
| `make` / PowerShell / Bash | Build script automation |

SDKs older than 475.001 (e.g. 192.001) lack `cellPadGetInfo2` and `cellPadSetPortSetting`; the build scripts then force-include `patches/compat_pad.h`, and skip it when the SDK's `cell/pad/libpad.h` declares them.

## Why PSL1GHT Cannot Be Used for VSH Plugins

PSL1GHT is an excellent open-source SDK, and it contains all the function signatures used here (`ioPadLddDataInsert`, `ioPadLddRegisterController`, `usbRegisterExtraLdd`, `usbInterruptTransfer`, `sysFs*`, etc.). However, it cannot build VSH plugins due to the **artifact binary format**.

A homebrew application is a `.self` (an executable ELF that the PS3 XMB launches as its own standalone process). A VSH plugin is a **PRX** (a relocable dynamic module with `.sceModuleInfo` and `module_start` / `module_stop` entry points, injected directly into the running `vsh.self` process).

Producing a PRX requires the `-mprx` compiler flag, which alters the code generation model (custom relocations and TOC handling) and generates the required SCE module sections. This is a proprietary extension SCE added to their GCC fork. The GCC shipped with `ps3toolchain` / PSL1GHT is standard GCC with newlib patches and **lacks `-mprx` support**.

All major VSH plugins in the PS3 scene (PS3xPAD, webMAN MOD, Akari, RouLetteVshMenu) rely on the PPU toolchain for this exact reason.

### The short answer

**The Cell SDK is the PS3 plugin toolchain.** It is not a workaround or an exotic requirement of this project: PS3xPAD, webMAN MOD, Akari, IRISMAN and every other HEN plugin you may have installed were compiled with it. "Building like any other HEN plugin" **is** building with the Cell SDK. There is no easier path that other developers use and that this document forgot to mention; they use this one.

The good news: the SDK ships a native Windows toolchain. You do not need WSL, Linux, or to build GCC from scratch. Once the environment is set up, `make` is all it takes.

### Alternatives that were checked

These were checked, not assumed:

| Attempt | Result |
|---|---|
| PSL1GHT | Has the whole API (`ioPadLddDataInsert`, `usbRegisterExtraLdd`, `lv2syscall4`), but **no rule that produces a `.prx`**. Only `%.a`, `%.o`, `%.elf`, `%.self`, `%.pkg` |
| `sprxlinker` | Despite the name, it does the opposite: it fixes up stubs so a homebrew can **call** system SPRX modules. It consumes them, it does not produce them |
| PSL1GHT forks (bucanero, bigboss, kakaroto) | Identical on this point |
| PSDK3v2 (Estwald) | Based on PSL1GHT. Same limitation |
| Open ELF-to-PRX converter | None exists for the PS3. `psp-prxgen` is PSP only; `SPRXPatcher` does something else |

## Setting Up the Environment on Windows

### 1. Get the SDK

Search for **"PS3 SDK 4.75"** or **"CELL SDK 3.41"**. Those are the two versions the scene uses; either one works for this project. It is Sony software from 2010, abandoned for over a decade, and it circulates in scene archives. It is not distributed by Sony and no link is given here.

You have the right package if, after extracting it, these paths exist:

```
<root>\host-win32\ppu\bin\ppu-lv2-gcc.exe
<root>\target\ppu\lib\libio_stub.a
<root>\target\ppu\lib\libusbd_stub.a
<root>\target\ppu\lib\libfs_stub.a
<root>\target\ppu\lib\libsysmodule_stub.a
<root>\samples\mk\sdk.makedef.mk
```

If any of them is missing, the package is incomplete; look for another one. The four `.a` files are exactly the ones this plugin uses.

The SDK used for this project came from the official installer, extracted without running it. The packages used, from inside `PS3 4.75 SDK Offline Installer/InstallFiles/`:

| Package | What it is for |
|---|---|
| `[157]/PS3_Toolchain_411-Win_420_001.zip` | **The compiler.** `ppu-lv2-gcc` is not in the base package of SDK 4.75 |
| `[155]/PS3_SDK-475_001.zip` | Headers and stubs (`libio_stub.a` and friends) |
| `[164]/PS3_SDK_Samples-475_001.zip` | `samples/mk/*.mk`, used by the Makefile |

Be careful with `unzip`: on some versions the glob `"cell/target/*"` does **not** recurse into subfolders, and silently extracts only the first level. Extract the whole zip or use explicit patterns per subdirectory.

### 2. Get scetool with the keys

`scetool` is **open source**: <https://github.com/naehrwert/scetool>. What does not come with it is the `keys` file, with the 3.55 retail keys, derived by the scene in 2011 and distributed the same way as the SDK.

Put `scetool.exe` and `keys` in the same folder, and put that folder in PATH.

naehrwert's repository only contains the source. The ready-made binary from the webMAN MOD repository works, but it is a **Cygwin** build and needs three DLLs that do not come with it:

```
cygwin1.dll       cygwin package
cygz.dll          zlib0 package
cyggcc_s-1.dll    libgcc1 package   <- easy to forget: it is a dependency
                                       of cygz.dll, not of scetool
```

They come from the Cygwin project and are freely redistributable, and the executable is 32-bit, so they must be the x86 versions. Without the third one, Windows returns `0xC0000135` (DLL not found) without saying which.

The keys live in `data/keys`, `data/ldr_curves` and `data/vsh_curves`, and scetool locates them through the `PS3` environment variable, which must point to the folder that **contains** `keys`.

### 3. Environment variables

In PowerShell, to test in the current session:

```powershell
$env:CELL_SDK = "C:\ps3sdk"
$env:PATH = "$env:PATH;C:\ps3sdk\host-win32\ppu\bin;C:\ps3sdk\host-win32\bin;C:\scetool"
```

Replace the paths with your own. To make them permanent, use
**Settings → System → About → Advanced system settings → Environment Variables**.

### 4. Check that it works

```powershell
ppu-lv2-gcc --version
```

```powershell
scetool --help
```

If both respond, you are ready.

## Compiling

### Windows (PowerShell)
```powershell
$env:CELL_SDK = "C:\path\to\cell_sdk"
$env:NPP_TOOLS = "C:\path\to\scetool_and_keys"   # omit to sign with make_fself (DEX only)
cd src
.\build.ps1 -Variant vsh            # -Variant game, -Debug, -Sign fself|scetool
```

### Linux / Git Bash
```bash
export CELL_SDK="/path/to/cell_sdk"
export NPP_TOOLS="/path/to/scetool_and_keys"   # omit to sign with make_fself (DEX only)
cd src
./build.sh
```

This produces `nativepadps3_vsh.sprx`, already signed and with the magic bytes checked. The script assumes the SDK is at `C:/ps3sdk/cell` and the tools at `C:/ps3sdk/tools`; if you keep them elsewhere, export `CELL_SDK` and `NPP_TOOLS`.

It deliberately does not use GNU make: the `make.exe` distributed with webMAN MOD is a **GUI** subsystem binary, so under Git Bash it returns no output and no usable exit code. Calling the compiler directly is simpler and much easier to debug.

### GNU Make
```bash
make            # builds nativepadps3_vsh.sprx
make game       # builds nativepadps3_game.sprx
make all-builds # builds both
make SIGN=fself # sign with the SDK's make_fself instead of scetool (DEX only)
```

On Windows, from PowerShell:

```powershell
cd C:\path\to\NativePadPS3\src
make
```

This produces `nativepadps3_vsh.sprx`.

### Checking the result

The file should be a few tens of KB and start with the `SCE\0` signature. To check:

```powershell
Format-Hex -Path nativepadps3_vsh.sprx -Count 4
```

It should show `53 43 45 00`. If it shows `7F 45 4C 46` (`ELF`), `scetool` did not run and the file **will not load**; on HEN, an invalid SPRX in `boot_plugins.txt` hangs the boot. Set up the safety net from [INSTALL_HEN.md](INSTALL_HEN.md#step-1--set-up-the-safety-net-do-this-before-anything-else) before testing.

## SPRX Signing (`make_fself`, DEX/debug consoles)

The SDK's `make_fself` turns the PRX into a fake-signed SELF, meant for DEX and debug firmware; a retail (CEX) console rejects it. It needs no keys and takes no auth ID. `build.ps1 -Sign fself` and `make SIGN=fself` force it; `build.sh` uses it whenever `NPP_TOOLS` is unset.

## SPRX Signing (`scetool`, CEX CFW and HEN)

Plugins must be signed with retail keys to execute on consumer consoles:

```bash
scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1070000052000001 \
        -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 \
        -e nativepadps3.prx nativepadps3_vsh.sprx
```

- **`-2 0A`**: Key revision 3.55. **Mandatory for PS3HEN.** Plugins signed with older revisions (e.g. `04`) will cause HEN to hang during boot.
- **`-3 1070000052000001`**: Application auth ID authorizing the module to run within the VSH process (`sys/internal + vsh/module`).
- **`-6 0003004000000000`**: Target firmware minimum.
- **`-A 0001000000000000`**: Application version.

The build script validates that the resulting file starts with the magic bytes `SCE\0` (`53 43 45 00`). An unsigned ELF module will hang HEN at boot.

## Installation

Copy the output files to your console's hard drive:
```text
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx
/dev_hdd0/plugins/nativepadps3/nativepadps3.txt
/dev_hdd0/plugins/nativepadps3/nativepadps3_devices.txt
```

Add the path to `/dev_hdd0/boot_plugins.txt`:
```text
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx
```

## Common Errors

| Error | Cause |
|---|---|
| `make: *** No rule to make target ... sdk.makedef.mk` | `CELL_SDK` wrong or not set |
| `ppu-lv2-gcc: command not found` | `host-win32\ppu\bin` is missing from PATH |
| `cannot find -lusbd_stub` | Incomplete SDK: `target\ppu\lib` is missing |
| `system_call_4` not declared | Add `#include <sys/syscall.h>` at the top of `padldd.c` and `main.c` |
| `cellPadGetRawData` not declared | Old SDK. It is only used by the diagnostics; delete the 4 lines that call it in `diag.c` |
| `scetool: command not found` | Not in PATH, or the `keys` file is missing next to it |
| `unused variable` warnings | Normal, you can ignore them |

If an error shows up that is not in this table, report it together with the output of `make`.

## In the Meantime

**PS3xPAD** has a build already signed for HEN, ready to download and install today ([GBAtemp](https://gbatemp.net/download/ps3xpad-3-55-signed-for-hen.37155/)). It gives you the PS button and game compatibility on a DualShock 4, without DualSense support and **without the analog stick curve fix**, which is the main complaint here. It works as a stopgap while you set up the environment, not as a replacement.
