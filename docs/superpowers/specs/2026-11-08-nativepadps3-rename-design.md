# NativePadPS3 rename: plugin files, config folder and boot trace

Status: designed 2026-11-07, not implemented. Branch `feat/nativepad-rename`
(from main 4478969). Inventory and decision record:
`.superpowers/rename-brainstorm-1.md` (git-ignored scratch; the inventory is
summarised in "Scope" below).

## Intent

What the user said: the project is now called NativePadPS3 (README, remote
`Desulation/NativePadPS3`), but the plugin still builds, deploys and configures
itself as `ds45pad`. "Can we change the sprx name to match the new project
name?" - and, asked how far to go: "Sprx plus config folder and trace". The old
config path must keep working as a fallback so existing setups do not break;
the console tools, `ps3_ready/` and the docs follow.

Decisions taken in brainstorming (all as recommended):

- Q1 naming style: all-lowercase `nativepadps3`, in the PS3 plugin convention.
- Q2 the in-binary module name and the PPU thread name change too.
- Q3 every console-visible string and debug tmp file carrying the old name
  changes too (toasts, diag header, TTY prefix, USB default device name,
  `*_diag.txt`, `*_lv2*.bin/.txt`).
- Q4 internal identifiers and the `DS45PAD - ...` banner comments stay; one note
  in `ds45pad.h` explains the historical prefix.
- Q5 deploy replaces the old `boot_plugins.txt` entry and touches nothing else
  on the console; nothing is deleted.

Assumptions (correct me if any is wrong):

- The user runs every console step; the agent never contacts 192.168.1.97.
- The tools only need the trace written by the build they deploy, so they read
  the new trace name only.
- Nobody depends on the SPRX's internal module name (`SYS_MODULE_INFO`); no tool
  or doc in this repo looks it up.

## Fixed names

| Item | Old | New |
|---|---|---|
| Build outputs | `ds45pad_vsh.sprx`, `ds45pad_vsh_debug.sprx`, `ds45pad_game.sprx`, `ds45pad_game_debug.sprx` | `nativepadps3_vsh.sprx`, `nativepadps3_vsh_debug.sprx`, `nativepadps3_game.sprx`, `nativepadps3_game_debug.sprx` |
| Build intermediates | `ds45pad.sym`, `ds45pad.prx`, `ds45pad.signed.tmp` | `nativepadps3.sym`, `nativepadps3.prx`, `nativepadps3.signed.tmp` |
| Release plugin on console | `/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx` | `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx` |
| Debug plugin on console (deploy tool) | `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx` | `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx` (plugins root, as today) |
| Data folder | `/dev_hdd0/plugins/ds45pad/` | `/dev_hdd0/plugins/nativepadps3/` |
| Config | `ds45pad.txt` | `nativepadps3.txt` |
| USB device list | `ds45pad_devices.txt` | `nativepadps3_devices.txt` |
| Stick calibration (written by the plugin) | `ds45pad_cal.txt` | `nativepadps3_cal.txt` |
| Kernel match cache | `kernel_cache.txt` | unchanged name, inside whichever data folder is resolved |
| Boot trace | `/dev_hdd0/tmp/ds45pad_boot.txt` | `/dev_hdd0/tmp/nativepadps3_boot.txt` |
| Diag log | `/dev_hdd0/tmp/ds45pad_diag.txt` | `/dev_hdd0/tmp/nativepadps3_diag.txt` |
| LV2 dumps (debug VSH only) | `/dev_hdd0/tmp/ds45pad_lv2.bin/.txt`, `ds45pad_lv2_b.bin/.txt` | `/dev_hdd0/tmp/nativepadps3_lv2.bin/.txt`, `nativepadps3_lv2_b.bin/.txt` |
| PRX module name | `SYS_MODULE_INFO(DS45PAD, 0, 1, 0)` | `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0)` |
| Main thread name | `"ds45pad"` | `"nativepadps3"` (stop thread `"ds45stop"` -> `"nppstop"`) |
| Toasts (NOTIFY = 1 only) | `DS45Pad Caricato / Loaded`, `DS45Pad Disattivato / Unloaded`, `DS45Pad Bluetooth Attivo / Active`, `DS45Pad: Bluetooth not supported on this kernel` | the same with `NativePadPS3` in place of `DS45Pad` |
| Diag header | `=== DS45PAD <version> ===` | `=== NativePadPS3 <version> ===` |
| TTY prefix (lv2dump) | `ds45pad: ` | `nativepadps3: ` |
| USB default device name | `DS45PAD device` | `NativePadPS3 device` |
| Shipped configs | `ps3_ready/plugins/ds45pad/ds45pad.txt`, `ds45pad_devices.txt` | `ps3_ready/plugins/nativepadps3/nativepadps3.txt`, `nativepadps3_devices.txt` (git mv; header comments updated) |
| Release boot entry | `ps3_ready/boot_plugins.txt`: `/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx` | `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx` |

Display strings use `NativePadPS3`; file names, paths, TTY prefix and thread
names use `nativepadps3`, mirroring the current `DS45Pad` toast vs `ds45pad`
file split. The version string (`DS45PAD_VERSION`, currently
`1.0.1-ps-usb-candidate[-debug]`) does not change.

## Legacy data-folder fallback

The plugin resolves its data folder **once**, at the start of `ds45_start`,
before `ds45_config_load`, and never again:

1. If `/dev_hdd0/plugins/nativepadps3/nativepadps3.txt` exists -> **new**
   folder: config, devices, calibration and kernel cache are all read and
   written under `/dev_hdd0/plugins/nativepadps3/` with the new file names.
   Trace marker `datadir_new`.
2. Else if `/dev_hdd0/plugins/ds45pad/ds45pad.txt` exists -> **legacy** folder:
   all four files under `/dev_hdd0/plugins/ds45pad/` with the **old** file
   names (`ds45pad.txt`, `ds45pad_devices.txt`, `ds45pad_cal.txt`,
   `kernel_cache.txt`). Trace marker `datadir_legacy`.
3. Else -> new folder (case 1) with no config; the existing
   `config_NOT_FOUND_using_defaults` marker follows as today. Trace marker
   `datadir_new`.

Existence is tested with `ds45_file_exists()` (already in `util.c`, faked in
`tests/fakes.c`). The whole set follows one decision so the calibration file is
never read from one folder and written to another, and a write never targets a
folder that does not exist (the plugin has no `cellFsMkdir`; a missing folder
makes writes fail silently). The trace and diag files and the LV2 dumps live in
`/dev_hdd0/tmp/` and always use the new names, whichever folder was resolved.

No prompt, no toast, no retry: the fallback is passive and only leaves the
marker in the boot trace (project standard: works like a DS3 or better,
verification passive only). An operator migrates by creating the new folder,
copying the config under its new name (and optionally the devices and
calibration files) - from then on the new folder wins even if the old one is
left in place.

### Code shape

- `src/ds45pad.h`: the fixed macros `DS45_DIR`, `DS45_CONFIG_FILE`,
  `DS45_DEVICE_FILE`, `DS45_CAL_FILE` and `kernel.h`'s `DS45_KERNEL_CACHE_FILE`
  are replaced by two string constants (`DS45_DATADIR_NEW`,
  `DS45_DATADIR_LEGACY`), an enum of the four file kinds, and
  `void ds45_paths_init(void);` / `const char *ds45_path(ds45_path_kind_t);`.
  `DS45_BOOT_FILE` and `DS45_DIAG_FILE` stay compile-time strings with the new
  names. A two-line comment states that `ds45`/`DS45PAD` is the historical
  internal prefix kept for stability and that the console-facing name is
  NativePadPS3.
- `src/paths.c` (new, both build variants: the game build also loads the
  config): the resolver and the accessor; the resolved paths are static
  strings, no allocation. It must be added to **all three** source lists
  (`Makefile` `SRCS_VSH`/`SRCS_GAME`, `build.ps1` `$sources`, `build.sh` both
  `SRCS`), since the two variants do not share a list.
- Call sites: `main.c` (config load), `usb.c` (device list), `sticks.c` (cal
  read and write), `kernel.c` (kernel cache).
- `main.c`: `ds45_paths_init()` right after the `start`/version markers;
  module name, thread names, toasts, diag header updated. `lv2dump.c`,
  `diag.c`, `usb.c`, `passthru.c` string updates per the table.

## Build scripts

`src/Makefile`, `src/build.ps1`, `src/build.sh`: outputs and intermediates per
the table; `CLEANFILES` lists the four new SPRX names (the old four may stay
listed for one cycle so `make clean` still removes stale builds). `.gitignore`:
add `nativepadps3_boot*.txt`, keep `ds45pad_boot*.txt` (old traces still get
pulled into `tools/dumps/`). `patches/build_linux_wine.sh` intermediates follow
(cosmetic; `OUT` is a parameter there). The signing parameters, auth ids, magic
check and the shim-blob build are untouched. `DS45_TOOLS` stays.

## Console tools

One shared definition in `tools/ps3_console_lib.ps1`: the trace path
`/dev_hdd0/tmp/nativepadps3_boot.txt` used by `Get-Trace`, the remote debug
plugin path, and `$PluginEntryPattern = 'ds45pad|nativepadps3'` for
`boot_plugins.txt` matching.

`ps3_console_deploy.ps1`:
- default `-Sprx ../src/nativepadps3_vsh_debug.sprx`; remote
  `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx`; size-check regex and backup
  file names follow.
- config upload to `/dev_hdd0/plugins/nativepadps3/nativepadps3.txt` with
  `--ftp-create-dirs` (the folder does not exist on a console that only ever
  ran ds45pad). The backup step downloads the new-path config if present, else
  the legacy one, and says which.
- `boot_plugins.txt`: **replace**, not append. Every line matching
  `$PluginEntryPattern` is dropped, the new remote path is added once, the file
  is re-read and the run fails unless exactly one line matches the pattern and
  it is the new path. Two listed plugins would install the kernel shim twice at
  boot; this is the highest-risk failure of the rename.
- trace: rename an existing `nativepadps3_boot.txt` with a stamp, as today;
  old `ds45pad_boot*.txt` files are not touched (Q5).
- everything else (reboot, wait for `loop`, peeks, power-off sites) unchanged.

`ps3_console_restore.ps1`: drop every line matching `$PluginEntryPattern`
(old and new names), fail if any survives; otherwise unchanged. The plugin
files and both config folders stay on the disk, as the script's contract says.

`ps3_console_test.ps1`: defaults, help text, `$trace`, `$dumpFiles` and the
btcheck output name follow the table. `ps3_console_trace.ps1`: help text only
(it uses `Get-Trace`). `ps3_console_linkkeys.ps1`: no change.

## Shipped files and docs

- `ps3_ready/`: `git mv plugins/ds45pad plugins/nativepadps3`, files renamed,
  header comments corrected, `boot_plugins.txt` entry updated.
- `README.md` lines about `ds45pad_devices.txt`, `ds45pad.txt`, the install
  paths, the `boot_plugins.txt` line and the build output; plus a short
  "Upgrading from ds45pad" paragraph: the old folder keeps working; to migrate,
  create `/dev_hdd0/plugins/nativepadps3/` and copy the config under its new
  name; replace the `boot_plugins.txt` line, never add a second one.
- `docs/BUILD.md` (title and paths), `COMPILAR.md`, `INSTALAR_CFW.md`,
  `INSTALAR_HEN.md` (also the stale `data/ds45pad.txt` reference and the stale
  toast text), `PEDIR_BUILD.md`, `DIAGNOSTICO.md`, `ANALOGICOS.md`,
  `ARQUITETURA.md`, `TESTE_PS3XPAD.md`: paths and file names per the table.

## Explicitly NOT changed

- Internal identifiers: `src/ds45pad.h` (file name and guard), `DS45_*` macros,
  `ds45_*` functions and types, `DS45_DEBUG` / `DS45_GAME` / `DS45_HOST_TEST`,
  `DS45PAD_VERSION`, the `DS45_TOOLS` environment variable. No console-side
  effect; renaming them would touch every file and every test.
- Banner comments `/* DS45PAD - ... */` in `src/`, `shim/`,
  `patches/compat_pad.h`, `tools/shim_pack.py`'s generated header comment,
  `tools/lv2scan.py`'s docstring.
- Historical records: everything under `docs/superpowers/specs/`,
  `docs/superpowers/plans/`, `docs/superpowers/notes/` (they describe the tree
  and the local dump files as they were and are), and `patches/ps_only.patch`
  (a diff against the old tree; editing it corrupts it).
- Upstream credits: "original DS45Pad" in `README.md` and `CREDITS.md` names
  the project at `github.com/Dobridp/DS45Pad`.
- Local dumps under `tools/dumps/` (git-ignored) keep their names.
- On the console: the old binary `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx`,
  the old folder `/dev_hdd0/plugins/ds45pad/`, old traces and dumps. Nothing is
  deleted; only `boot_plugins.txt` lines are edited.

## Testing

### Host tests (`pwsh tests/run_tests.ps1`, clang, no SDK)

New `tests/test_paths.c` (`/* SOURCES: paths.c */`), with the in-memory file
fakes:

1. New config present (old present or absent): all four `ds45_path()` values
   are under `/dev_hdd0/plugins/nativepadps3/` with the new names.
2. Only the old config present: all four under `/dev_hdd0/plugins/ds45pad/`
   with the old names.
3. Neither present: new folder, new names.
4. Resolution happens once: setting the old file after `ds45_paths_init()` does
   not change the answer.
5. The trace marker for each case (`datadir_new` / `datadir_legacy`), captured
   with a test-local `ds45_boot_mark` override (the fake is weak).

`tests/test_lv2dump.c`: the TTY prefix and the dump paths follow the table;
everything else unchanged. All existing tests stay green.

Grep gate (part of the implementation's verification, not a committed test):
`rg -i ds45pad src tools ps3_ready README.md docs/*.md` must list only: the
legacy fallback constant and the legacy file names in `paths.c`, the
`ds45pad.h` file name (includes, guard, note), the banner comments kept by Q4,
the upstream credits, `DS45_TOOLS`, and the "Upgrading from ds45pad" text.

### Build

Both variants, release and debug, build with `build.ps1`; the four new SPRX
names appear, pass the `SCE\0` magic check. `strings` on
`nativepadps3_vsh_debug.sprx`: the new paths are present; the only old path is
`/dev_hdd0/plugins/ds45pad/ds45pad.txt` plus the legacy folder constant.

### Console stage (the controller runs it with the user; DEX 4.92)

This is a separate, explicit stage after the code and host tests are done. The
user runs the commands; the agent never contacts the console.

1. Before: `boot_plugins.txt` still lists `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx`;
   the old folder holds the live config; no new folder.
2. `pwsh tools/ps3_console_deploy.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`
   with the default config. Expected: backup says it took the legacy config; the
   new folder is created and the config uploaded; `boot_plugins.txt` shows
   exactly one plugin line, the new one; the console reboots; the new trace
   appears and the script reports "Plugin in its loop".
3. Trace check (`ps3_console_trace.ps1` or the deploy output): `start`,
   version, `datadir_new` (deploy uploaded the new config), `config_bytes`,
   `kshim` lines as before, `loop`, `shimdbg` lines. The DualSense connects and
   behaves exactly as before the rename (DS3 route, rumble, power-off).
4. Legacy path check: the user removes (renames) `nativepadps3.txt` on the
   console via FTP, reboots through webMAN; the trace shows `datadir_legacy`
   and `config_bytes` from the old `ds45pad.txt`; the pad behaves the same.
   Then the user restores the new config (or leaves the legacy one; both are
   valid end states).
5. `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`:
   `boot_plugins.txt` holds no plugin line of either name, reboot, hooks stock,
   `CLEAN`. The stage ends here, with the console back to stock.

Each step's trace is kept under `tools/dumps/` (git-ignored). Results are
recorded in a dated note and the Status line of this spec, as the earlier
specs do.

## Risks

1. **Two boot entries** after a naive deploy (old + new path) -> two plugins,
   two kernel-shim installs on the same hooks. Mitigation: deploy replaces and
   verifies exactly one entry; restore strips both names. The loader's handling
   of two modules is deliberately not relied on (whether a duplicate module
   name is refused is unverified, and the names now differ anyway).
2. **Config silently not found** under the new folder -> defaults ->
   `ENABLE_SHIM` off -> DS3 route gone, looking like a regression. Mitigation:
   the legacy fallback plus the `datadir_*` marker so the trace says which
   folder was used.
3. **Missing new folder**: no `cellFsMkdir` in the plugin; cal writes into a
   missing folder fail silently; FTP `-T` into it fails. Mitigation: in cases
   1 and 2 the resolved folder exists by construction (its config was found
   there); in case 3 (no config anywhere) writes may fail silently, exactly as
   today when `/dev_hdd0/plugins/ds45pad/` is missing - not a regression.
   Deploy uses `--ftp-create-dirs`; docs say "create the folder".
4. **Trace name mismatch** between plugin and tools -> deploy waits 3 min and
   reports "plugin did not reach its loop" though the plugin runs. Mitigation:
   one definition in `ps3_console_lib.ps1`; the grep gate.
5. **Path typo** in `boot_plugins.txt` or in a build script -> the plugin is
   never loaded and there is no trace at all. Mitigation: the console stage's
   step 2 expects the new trace to appear; the restore step is the way back.
6. **Source lists**: `paths.c` missing from one of the three build scripts or
   from the game variant -> link error (loud) or a game build that cannot load
   its config (quiet). Mitigation: all four variants are built in the Build
   check.
7. Case-sensitive HDD paths: all names are lowercase by decision.
8. Old traces, dumps and the old binary remain on the console by design; a
   reader of `/dev_hdd0/tmp` sees both name families until the user cleans up.
