# Building NativePadPS3

## Prerequisites

Building a VSH SPRX plugin requires the **Sony Cell SDK PPU toolchain** (`ppu-lv2-gcc`). Signing uses either the SDK's own `make_fself` or `scetool`.

| Tool | Notes |
|---|---|
| Cell SDK (PPU) | `ppu-lv2-gcc` in PATH or specified via `CELL_SDK` environment variable |
| `make_fself` (SDK) | Default when `DS45_TOOLS` is unset. Produces a fake-signed SPRX that loads on **DEX/debug firmware only** |
| `scetool` | Used when `DS45_TOOLS` is set. Signs with 3.55 retail keys (`Revision 0A`) so the SPRX loads on CEX CFW and PS3HEN |
| `make` / PowerShell / Bash | Build script automation |

SDKs older than 475.001 (e.g. 192.001) lack `cellPadGetInfo2` and `cellPadSetPortSetting`; the build scripts then force-include `patches/compat_pad.h`, and skip it when the SDK's `cell/pad/libpad.h` declares them.

## Why PSL1GHT Cannot Be Used for VSH Plugins

PSL1GHT is an excellent open-source SDK, and it contains all the function signatures used here (`ioPadLddDataInsert`, `ioPadLddRegisterController`, `usbRegisterExtraLdd`, `usbInterruptTransfer`, `sysFs*`, etc.). However, it cannot build VSH plugins due to the **artifact binary format**.

A homebrew application is a `.self` (an executable ELF that the PS3 XMB launches as its own standalone process). A VSH plugin is a **PRX** (a relocable dynamic module with `.sceModuleInfo` and `module_start` / `module_stop` entry points, injected directly into the running `vsh.self` process).

Producing a PRX requires the `-mprx` compiler flag, which alters the code generation model (custom relocations and TOC handling) and generates the required SCE module sections. This is a proprietary extension SCE added to their GCC fork. The GCC shipped with `ps3toolchain` / PSL1GHT is standard GCC with newlib patches and **lacks `-mprx` support**.

All major VSH plugins in the PS3 scene (PS3xPAD, webMAN MOD, Akari, RouLetteVshMenu) rely on the PPU toolchain for this exact reason.

## Compiling

### Windows (PowerShell)
```powershell
$env:CELL_SDK = "C:\path\to\cell_sdk"
$env:DS45_TOOLS = "C:\path\to\scetool_and_keys"   # omit to sign with make_fself (DEX only)
cd src
.\build.ps1 -Variant vsh            # -Variant game, -Debug, -Sign fself|scetool
```

### Linux / Git Bash
```bash
export CELL_SDK="/path/to/cell_sdk"
export DS45_TOOLS="/path/to/scetool_and_keys"   # omit to sign with make_fself (DEX only)
cd src
./build.sh
```

### GNU Make
```bash
make            # builds nativepadps3_vsh.sprx
make game       # builds nativepadps3_game.sprx
make all-builds # builds both
make SIGN=fself # sign with the SDK's make_fself instead of scetool (DEX only)
```

## SPRX Signing (`make_fself`, DEX/debug consoles)

The SDK's `make_fself` turns the PRX into a fake-signed SELF, meant for DEX and debug firmware; a retail (CEX) console rejects it. It needs no keys and takes no auth ID. `build.ps1 -Sign fself` and `make SIGN=fself` force it; `build.sh` uses it whenever `DS45_TOOLS` is unset.

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
