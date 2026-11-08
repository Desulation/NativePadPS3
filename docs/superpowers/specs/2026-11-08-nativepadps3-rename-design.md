# NativePadPS3 rename: plugin files, config folder, boot trace, and the code prefix

Status: designed 2026-11-07, amended the same day (scope widened to the whole
code prefix, see "Amendment 1"), not implemented. Branch
`feat/nativepad-rename` (from main 4478969). Inventory and decision record:
`.superpowers/rename-brainstorm-1.md` (git-ignored scratch; the inventory is
summarised in "Scope" below).

Identifiers in this document are written in their **pre-rename** form
(`ds45pad.h`, `DS45_*`, `ds45_*`); step 2 maps every one of them by the rule in
"Amendment 1". Step 1 is implemented against the current identifiers.

## Intent

What the user said: the project is now called NativePadPS3 (README, remote
`Desulation/NativePadPS3`), but the plugin still builds, deploys and configures
itself as `ds45pad`. "Can we change the sprx name to match the new project
name?" - asked how far to go: "Sprx plus config folder and trace", and, after
approving the first version of this spec: "Anything aside from credits should
be renamed from ds45 to npp." The old config path must keep working as a
fallback so existing setups do not break; the console tools, `ps3_ready/` and
the docs follow.

Decisions taken in brainstorming:

- Q1 naming style: all-lowercase `nativepadps3` on the console, in the PS3
  plugin convention.
- Q2 the in-binary module name and the PPU thread names change too.
- Q3 every console-visible string and debug tmp file carrying the old name
  changes too (toasts, diag header, TTY prefix, USB default device name,
  `*_diag.txt`, `*_lv2*.bin/.txt`).
- Q4 (reversed by Amendment 1): internal identifiers, file names and banner
  comments are renamed from `ds45` to `npp`, as a separate mechanical step.
  There is no "historical prefix" note: nothing historical is left in the code.
- Q5 deploy replaces the old `boot_plugins.txt` entry and touches nothing else
  on the console; nothing is deleted.
- History stays as written: older specs, plans and notes under
  `docs/superpowers/`, `patches/ps_only.patch`, and the upstream credits.

Assumptions (correct me if any is wrong):

- The user runs every console step; the agent never contacts 192.168.1.97.
- The tools only need the trace written by the build they deploy, so they read
  the new trace name only.
- Nobody depends on the SPRX's internal module name (`SYS_MODULE_INFO`); no tool
  or doc in this repo looks it up.
- Values that are bytes on the console or on disk (the shim data-block magic,
  the kernel-cache file magic, the legacy folder and file names) keep their
  bytes; only the identifiers that name them are renamed.

## Two steps, two kinds of commit

- **Step 1 - functional rename**: on-console names, the legacy data-folder
  fallback, tools, `ps3_ready/`, docs. Hand-written, reviewed as a normal
  change, with the host test for the fallback. Sections "Fixed names" through
  "Shipped files and docs".
- **Step 2 - mechanical prefix rename** (`ds45` -> `npp`): scripted,
  reproducible, in its own commit(s) after step 1 so the functional diff stays
  readable. Section "Amendment 1".

Both steps share the gates in "Testing"; step 2 adds the blob byte-identity
gate.

## Fixed names (step 1)

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
| Kernel match cache | `kernel_cache.txt` | unchanged name, inside whichever data folder is resolved; its first-line magic `DS45K1` is unchanged (on-disk format) |
| Boot trace | `/dev_hdd0/tmp/ds45pad_boot.txt` | `/dev_hdd0/tmp/nativepadps3_boot.txt` |
| Diag log | `/dev_hdd0/tmp/ds45pad_diag.txt` | `/dev_hdd0/tmp/nativepadps3_diag.txt` |
| LV2 dumps (debug VSH only) | `/dev_hdd0/tmp/ds45pad_lv2.bin/.txt`, `ds45pad_lv2_b.bin/.txt` | `/dev_hdd0/tmp/nativepadps3_lv2.bin/.txt`, `nativepadps3_lv2_b.bin/.txt` |
| PRX module name | `SYS_MODULE_INFO(DS45PAD, 0, 1, 0)` | `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0)` |
| Thread names | `"ds45pad"`, `"ds45stop"` | `"nativepadps3"`, `"nppstop"` |
| Toasts (NOTIFY = 1 only) | `DS45Pad Caricato / Loaded`, `DS45Pad Disattivato / Unloaded`, `DS45Pad Bluetooth Attivo / Active`, `DS45Pad: Bluetooth not supported on this kernel` | the same with `NativePadPS3` in place of `DS45Pad` |
| Diag header | `=== DS45PAD <version> ===` | `=== NativePadPS3 <version> ===` |
| TTY prefix (lv2dump) | `ds45pad: ` | `nativepadps3: ` |
| USB default device name | `DS45PAD device` | `NativePadPS3 device` |
| webMAN popup in `ps3_console_linkkeys.ps1` | `DS45: power the pads on now` | `NativePadPS3: power the pads on now` |
| Shipped configs | `ps3_ready/plugins/ds45pad/ds45pad.txt`, `ds45pad_devices.txt` | `ps3_ready/plugins/nativepadps3/nativepadps3.txt`, `nativepadps3_devices.txt` (git mv; header comments updated) |
| Release boot entry | `ps3_ready/boot_plugins.txt`: `/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx` | `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx` |

Display strings use `NativePadPS3`; file names, paths, TTY prefix and thread
names use `nativepadps3`, mirroring the current `DS45Pad` toast vs `ds45pad`
file split. The version string value (`1.0.1-ps-usb-candidate[-debug]`) does
not change.

## Legacy data-folder fallback (step 1)

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

The legacy literals (`"/dev_hdd0/plugins/ds45pad"`, `"ds45pad.txt"`,
`"ds45pad_devices.txt"`, `"ds45pad_cal.txt"`) are **data the fallback must
read**. They live in one place (`paths.c`), each under a comment that says
`legacy on-console name, read-only compatibility, do not rename`, and they are
the only `ds45` strings step 2 leaves in `src/`.

### Code shape

- `src/ds45pad.h`: the fixed macros `DS45_DIR`, `DS45_CONFIG_FILE`,
  `DS45_DEVICE_FILE`, `DS45_CAL_FILE` and `kernel.h`'s `DS45_KERNEL_CACHE_FILE`
  are replaced by two string constants (`DS45_DATADIR_NEW`,
  `DS45_DATADIR_LEGACY`), an enum of the four file kinds, and
  `void ds45_paths_init(void);` / `const char *ds45_path(ds45_path_kind_t);`.
  `DS45_BOOT_FILE` and `DS45_DIAG_FILE` stay compile-time strings with the new
  names.
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

## Build scripts (step 1)

`src/Makefile`, `src/build.ps1`, `src/build.sh`: outputs and intermediates per
the table; `CLEANFILES` lists the four new SPRX names (the old four may stay
listed for one cycle so `make clean` still removes stale builds). `.gitignore`:
add `nativepadps3_boot*.txt`, keep `ds45pad_boot*.txt` (old traces still get
pulled into `tools/dumps/`). `patches/build_linux_wine.sh` intermediates follow
(`OUT` is a parameter there). The signing parameters, auth ids, magic check and
the shim-blob build are untouched.

## Console tools (step 1)

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
(it uses `Get-Trace`). `ps3_console_linkkeys.ps1`: popup text only.

## Shipped files and docs (step 1)

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
  Product prose `DS45Pad`/`DS45PAD` in these (non-history) docs ->
  `NativePadPS3`.

## Amendment 1 (2026-11-07): the code prefix `ds45` -> `npp` (step 2)

### Rule

One ordered replacement list, applied to every tracked text file outside the
allowlist, longest match first so a shorter rule never splits a longer name:

| Pattern | Replacement | Examples |
|---|---|---|
| `DS45PAD_VERSION`, `DS45PAD_H`, `DS45PAD_HOOK_H`, any `DS45PAD_<X>` | `NPP_<X>` | `NPP_VERSION`, `NPP_H` |
| `DS45PAD` (bare, banners and prose in code/tools) | `NPP` | `/* NPP - diagnostics. */`, `NPP kernel shim blob - GENERATED ...` |
| `DS45Pad`, `Ds45Pad` (product prose) | `NativePadPS3` | `lv2scan.py` docstring, non-history docs, UI text |
| `ds45pad_<x>` (file names, intermediates) | `npp_<x>` | none remain after step 1 except the legacy literals, which are excluded |
| `ds45pad` (file name, bare) | `npp` | `src/ds45pad.h` -> `src/npp.h`, `#include "npp.h"` |
| `DS45_<X>` | `NPP_<X>` | `NPP_DEBUG`, `NPP_GAME`, `NPP_HOST_TEST`, `NPP_TOOLS`, `NPP_SHIM_MAGIC`, `NPP_SHIM_OFF_DATA`, `NPP_SCAN_MAX_INSNS`, `NPP_COMPAT_PORTS`, `NPP_TEST_FAKES_H` |
| `ds45_<x>` | `npp_<x>` | `npp_config_t`, `npp_start`, `npp_shim_data`, `npp_state.h`, `npp_scan_pattern`, `npp_shim_blob[]` |
| `ds45` (bare, remaining identifiers) | `npp` | `ds45stop` -> `nppstop` (already in step 1) |

Files renamed with `git mv` in the same commit as their content change (so
`git log --follow` works): `src/ds45pad.h` -> `src/npp.h`,
`src/ds45_state.h` -> `src/npp_state.h`. No other tracked file carries the
prefix in its name after step 1. The `DS45_TOOLS` environment variable becomes
`NPP_TOOLS` in `build.ps1`, `build.sh`, `Makefile` comments and `docs/BUILD.md`.

### Cross-boundary lookups that must move in lockstep

These are places where one side names an identifier of the other as text. A
missed one is either a loud build failure or, worse, a quiet fallback.

| Lookup | Sides | Failure if missed |
|---|---|---|
| ELF symbol `ds45_shim_data` | `shim/shim_abi.h` + `shim/entry.c` (the C object **and** the inline-asm string literals at lines 349-350, 432-433, 498-499), `tools/shim_pack.py` 318 `find("ds45_shim_data")`, `tools/test_shim_pack.py` synthetic ELF symbol | loud: the packer raises, the link fails |
| Generated header names `DS45_SHIM_BLOB_LEN`, `DS45_SHIM_RELOC_COUNT`, `DS45_SHIM_OFF_*`, `ds45_shim_blob[]`, `ds45_shim_relocs[]`, guard `DS45_SHIM_BLOB_H` | emitted by `tools/shim_pack.py` 378-423, consumed by `src/kshim.c`, `tests/test_kshim.c`, asserted by `tools/test_shim_pack.py` 325-339, 408-409, 713-717 | loud in C; loud in the packer tests |
| `DS45_SHIM_OFF_POWEROFF` / `_TEARDOWN` parsed from `src/shim_blob.h` by regex | `tools/ps3_console_lib.ps1` 66 | **quiet**: the regex finds nothing and the script silently uses the hard-coded DEX 4.92 offsets. Step 2 also makes this loud: when `shim_blob.h` exists but the pattern does not match, throw. |
| C identifiers emitted as text by `tools/lv2scan.py` (`ds45_insn_match_t`, `ds45_scan_insns`, `ds45_scan_pattern_t`, `ds45_scan_pattern`, `ds45_kernel_table`) and its constants `DS45_SCAN_*`, `DS45_FP_LEN`, `DS45_INSN_*`, `DS45_REPORT_OFF_MAX`, `DS45_LV2_*` | `tools/lv2scan.py`, `tools/tests/test_lv2scan.py`, `src/kernel.h`/`kernel.c` | loud in the Python tests; a stale generator would emit C that no longer compiles when pasted |
| `/* SOURCES: ... */` and `/* CFLAGS: -DDS45_DEBUG */` header lines | each `tests/test_*.c`, parsed by `tests/run_tests.ps1` | loud: a test built without the flag fails to compile or fails |
| `-DDS45_HOST_TEST` | `tests/run_tests.ps1` and every `#ifdef DS45_HOST_TEST` | loud |

### Literals whose bytes stay (rename the identifier, keep the value)

Found by searching every string literal and numeric constant for `ds45`/`DS45`:

1. `shim/shim_abi.h` 18: `#define DS45_SHIM_MAGIC 0x4453343553484D31ULL /* "DS45SHM1" */`.
   The value is the blob data block's first qword (`shim/entry.c` 37), the
   installer compares it against a blob already resident in kernel RAM
   (`src/kshim.c` 548) and the data block it builds (572). Identifier ->
   `NPP_SHIM_MAGIC`; **value, its comment and the test literal `"DS45SHM1"`
   in `tests/test_ds3report.c` 1158-1160 stay**. Changing the value would
   change the blob bytes and make a running blob from a previous build
   unrecognisable.
2. `src/kernel.c` 58: `#define CACHE_MAGIC "DS45K1"` - first token of
   `kernel_cache.txt` on the console, plus the `DS45K1`/`DS45K0`/`ds45k1`
   literals in `tests/test_kernel.c` 142-251. **Bytes stay** (on-disk format;
   the legacy folder's cache must still parse). The macro name has no prefix
   and is unchanged.
3. The four legacy on-console names in `src/paths.c` (see the fallback
   section). **Bytes stay**, labelled legacy.
4. `tests/test_shim_pack.py` golden numbers (`BLOB_LEN 704`, `OFF_DATA 0x230`,
   ...) are values, not names; they stay. Only the macro names in the
   `assertIn` strings move.

No `ds45`/`DS45` was found in: config keys, the devices-file or cal-file
formats, trace markers, the shim data block other than the magic, the shim
linker script (banner only), protocol constants. If the implementation's grep
finds another byte-bearing literal, it is listed in the commit message and
kept, not renamed.

### Method

A throwaway script in the agent's scratchpad (not committed), driven by:
(a) the ordered replacement table above; (b) an explicit file list = `git
ls-files` minus the allowlist (`CREDITS.md`, `patches/ps_only.patch`,
`docs/superpowers/**` except this spec, `LICENSE`) minus binary/ignored files;
(c) an explicit exclusion list of the byte-preserving lines (matched by file +
exact line content, so a drift makes the script stop). It performs the two
`git mv`s, applies the replacements, and prints a per-file count. The
replacement table and the exclusion list are reproduced in the commit
message. The README's upstream-credit lines (9 and 175) are excluded by exact
line content; the "Upgrading from ds45pad" paragraph and the `.gitignore`
pattern `ds45pad_boot*.txt` are excluded as legacy names.

### Gates for step 2 (all must pass before the commit)

1. **Blob byte identity.** Build the shim blob from the last step-1 commit and
   from the step-2 tree with the same clang/lld/python; compare the packed
   output: `BLOB_LEN` equal, every `OFF_*` value equal (names differ; compare
   values), the `blob[]` byte array and the `relocs[]` array identical
   (SHA-256 of the extracted arrays, or a diff of the two generated headers
   after stripping the `DS45_`/`NPP_` prefixes and the banner line - both
   methods must agree). Also `sha256(shim/shim.elf .text + .data + .toc
   section bytes)` equal. Symbol renames must not move a single byte.
2. **Host suite** `pwsh tests/run_tests.ps1`: every test builds and passes
   (including the new `test_paths.c`).
3. **Python/packer tests**: `python -I -m pytest tools/test_shim_pack.py
   tools/test_shim_stubs.py tools/tests/` and `pwsh
   tools/test_check_no_proprietary.ps1` pass.
4. **Four SDK builds** (`build.ps1 -Variant vsh|game`, with and without
   `-Debug`) produce the four `nativepadps3_*.sprx` with the `SCE\0` magic;
   `strings` on the debug VSH build shows no `ds45` other than the legacy
   folder/file names and `DS45SHM1`/`DS45K1`.
5. **Grep gate**: `rg -i ds45` over tracked files lists only: the upstream
   credits (`README.md` lines 9 and 175, `CREDITS.md`), `patches/ps_only.patch`,
   `docs/superpowers/` history, the labelled legacy literals in `src/paths.c`,
   `DS45_SHIM_MAGIC`'s value comment and `"DS45SHM1"` in `test_ds3report.c`,
   `"DS45K1"` in `kernel.c` and `test_kernel.c`, the "Upgrading from ds45pad"
   paragraph, the `.gitignore` legacy pattern, and this spec. Anything else is
   a missed rename.
6. **Tools dry check**: `pwsh -NoProfile -Command '. tools/ps3_console_lib.ps1'`
   with `$Ip` set to a placeholder loads without error and, with a generated
   `src/shim_blob.h` present, reports the parsed power-off offsets (not the
   hard-coded fallback).

### Decisions confirmed by the user (2026-11-08)

- (a) Both byte-bearing magics keep their bytes under new names:
  `NPP_SHIM_MAGIC = 0x4453343553484D31` ("DS45SHM1") and `CACHE_MAGIC "DS45K1"`.
- (b) `DS45_TOOLS` -> `NPP_TOOLS`. For one cycle `build.ps1` and `build.sh`
  fail loudly with a clear message when `DS45_TOOLS` is set and `NPP_TOOLS` is
  not. Those check lines (and the tool self-test cases that exercise them)
  necessarily name `DS45_TOOLS` and are part of the gate-5 allowlist.
- (c) History under `docs/superpowers/` stays as written.
- (d) The stop thread is `"nppstop"`.

Plan reading (for review): besides `src/paths.c`, the legacy on-console names
also have to stay readable where the tools and tests match or assert them
(`$PluginEntryPattern` and `$LegacyConfigFile` in `tools/ps3_console_lib.ps1`,
the old-path fixtures in `tools/test_console_lib.ps1` and `tests/test_paths.c`).
They carry the same `legacy on-console name` label, are excluded by exact line,
and count as "labelled legacy literals" in gate 5. The spec's two console
stages (after step 1 and after step 2) are both kept.

## Explicitly NOT changed

- Upstream credits: "original DS45Pad" in `README.md` (lines 9 and 175) and
  `CREDITS.md` name the project at `github.com/Dobridp/DS45Pad`.
- `patches/ps_only.patch`: a diff against the old tree; editing it corrupts it.
- Historical records under `docs/superpowers/specs/`, `docs/superpowers/plans/`,
  `docs/superpowers/notes/` (everything older than this spec): they describe
  the tree and the local dump files as they were. This spec is the only
  `docs/superpowers` file step 2 touches (its own text stays in pre-rename
  form by design, see the note at the top).
- Byte-bearing values: `0x4453343553484D31` ("DS45SHM1"), `"DS45K1"`, the
  legacy on-console folder and file names.
- Local dumps under `tools/dumps/` (git-ignored) keep their names.
- On the console: the old binary `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx`,
  the old folder `/dev_hdd0/plugins/ds45pad/`, old traces and dumps. Nothing is
  deleted; only `boot_plugins.txt` lines are edited.
- The version string value, the signing parameters, auth ids, and every
  kernel address/offset.

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
everything else unchanged. All existing tests stay green after step 1 and
again after step 2.

### Build (after step 1 and again after step 2)

Both variants, release and debug, build with `build.ps1`; the four new SPRX
names appear and pass the `SCE\0` magic check. `strings` on
`nativepadps3_vsh_debug.sprx`: the new paths are present; the only old path is
`/dev_hdd0/plugins/ds45pad/ds45pad.txt` plus the legacy folder constant and the
legacy file names.

### Console stage (the controller runs it with the user; DEX 4.92)

A separate, explicit stage after each step's code and host tests are done.
The user runs the commands; the agent never contacts the console.

**After step 1 (functional rename):**

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
   Then the user restores the new config.
5. `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`:
   `boot_plugins.txt` holds no plugin line of either name, reboot, hooks stock,
   `CLEAN`.

**After step 2 (prefix rename) - the post-rename debug build must still load:**

6. `pwsh tools/ps3_console_deploy.ps1 -Ip <console> -Dest tools/dumps/dex492/rename2`.
   Expected: identical trace shape to step 3 (`start`, version, `datadir_new`,
   `config_bytes`, `kshim` accepted with the same magic/version, `loop`,
   `shimdbg`), the deploy's power-off site read-back shows the parsed offsets
   (gate 6), the pad behaves the same. A missed rename in a name the plugin or
   the tools look up would show here as no trace, no `loop`, or a kshim
   refusal.
7. `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename2`:
   `CLEAN`. The stage ends with the console back to stock.

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
   never loaded and there is no trace at all. Mitigation: the console stage
   expects the new trace to appear; the restore step is the way back.
6. **Source lists**: `paths.c` missing from one of the three build scripts or
   from the game variant -> link error (loud) or a game build that cannot load
   its config (quiet). Mitigation: all four variants are built.
7. Case-sensitive HDD paths: all names are lowercase by decision.
8. Old traces, dumps and the old binary remain on the console by design; a
   reader of `/dev_hdd0/tmp` sees both name families until the user cleans up.
9. **Step 2 over-replacement** of a byte-bearing literal (`DS45SHM1` value or
   comment, `DS45K1`, the legacy paths, the credit lines): the exclusion list
   matches exact line content and the blob-identity gate plus the grep gate
   catch a slip; `test_kernel.c` and `test_ds3report.c` assert the bytes.
10. **Step 2 under-replacement** in a cross-boundary lookup (packer symbol
    name, header regex in `ps3_console_lib.ps1`, lv2scan generated names):
    the lockstep table above; the quiet one (`ps3_console_lib.ps1` regex) is
    made loud; console step 6 is the end-to-end check.
11. **Mechanical rename hitting a substring**: `ds45` inside a longer
    identifier that is not ours, or `DS45` inside the hex `0x4453...` (it is
    not: the hex has no `DS45` text) - the ordered table and a review of the
    per-file counts against the pre-rename `rg -c` counts. A test that the
    script is idempotent (second run changes nothing) is part of the method.
12. **Blame/history**: `git mv` in the same commit as the content change keeps
    `--follow` working; the mechanical commit is separate from the functional
    one so reviewers can skip it.
