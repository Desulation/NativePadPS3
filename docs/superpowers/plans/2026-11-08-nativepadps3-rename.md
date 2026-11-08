# NativePadPS3 Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (the user's standing choice of execution method; do not ask, never execute inline with executing-plans) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The plugin builds, deploys, configures and traces itself as `nativepadps3` instead of `ds45pad`, while a console that only has the old `/dev_hdd0/plugins/ds45pad/` config folder keeps working unchanged.

**Architecture:** A new pure module `src/paths.c` resolves the data folder once at plugin start (new folder if its config exists, else legacy folder if its config exists, else new) and hands out the four data-file paths; every caller of the old fixed macros asks it instead. Console-visible strings, build outputs, console tools, `ps3_ready/` and docs switch to the new names; the tools share one definition of the trace path and the boot-entry pattern, and deploy *replaces* the `boot_plugins.txt` entry instead of appending.

**Tech Stack:** C (Cell SDK PPU gcc for the plugin, LLVM clang for host tests with `tests/fakes.c`), PowerShell 7 (build script, console tools, tool self-test), make/bash (alternate build scripts).

**Spec:** [docs/superpowers/specs/2026-11-08-nativepadps3-rename-design.md](../specs/2026-11-08-nativepadps3-rename-design.md) (approved, binding). Executors read the spec and this plan. Inventory and decisions Q1-Q5: `.superpowers/rename-brainstorm-1.md` (git-ignored scratch).

## Global Constraints

- Branch `feat/nativepad-rename`. Do not switch branches, do not push. Never contact the console 192.168.1.97 or any other network device (Tasks 1-6 are fully offline; Task 7 is run by the controller with the user).
- Naming (Q1): all-lowercase `nativepadps3` for file names, paths, TTY prefix and thread names; display strings use `NativePadPS3`.
- Build outputs: `nativepadps3_vsh.sprx`, `nativepadps3_vsh_debug.sprx`, `nativepadps3_game.sprx`, `nativepadps3_game_debug.sprx`. Intermediates: `nativepadps3.sym`, `nativepadps3.prx`, `nativepadps3.signed.tmp`.
- Release plugin on console: `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`. Debug plugin (deploy tool): `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx` (plugins root, as today).
- Data folder `/dev_hdd0/plugins/nativepadps3/`: config `nativepadps3.txt`, USB device list `nativepadps3_devices.txt`, stick calibration `nativepadps3_cal.txt`, kernel match cache `kernel_cache.txt` (unchanged name, inside whichever data folder is resolved).
- Legacy data folder `/dev_hdd0/plugins/ds45pad/` with the **old** file names `ds45pad.txt`, `ds45pad_devices.txt`, `ds45pad_cal.txt`, `kernel_cache.txt`.
- Boot trace `/dev_hdd0/tmp/nativepadps3_boot.txt`; diag log `/dev_hdd0/tmp/nativepadps3_diag.txt`; LV2 dumps (debug VSH only) `/dev_hdd0/tmp/nativepadps3_lv2.bin/.txt`, `nativepadps3_lv2_b.bin/.txt`. These always use the new names, whichever data folder is resolved.
- PRX module name `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0)`. Main thread `"nativepadps3"`, stop thread `"nppstop"`.
- Toasts: `NativePadPS3 Caricato / Loaded`, `NativePadPS3 Disattivato / Unloaded`, `NativePadPS3 Bluetooth Attivo / Active`, `NativePadPS3: Bluetooth not supported on this kernel`. Diag header `=== NativePadPS3 <version> ===`. TTY prefix `nativepadps3: `. USB default device name `NativePadPS3 device`.
- Trace markers: `datadir_new` / `datadir_legacy`; the existing `config_NOT_FOUND_using_defaults` stays.
- Release boot entry (`ps3_ready/boot_plugins.txt`): `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`.
- Tools: `$PluginEntryPattern = 'ds45pad|nativepadps3'` in `tools/ps3_console_lib.ps1`.
- Unchanged (Q4 / "Explicitly NOT changed"): `src/ds45pad.h` file name and `DS45PAD_H` guard, `DS45_*` macros, `ds45_*` functions and types, `DS45_DEBUG` / `DS45_GAME` / `DS45_HOST_TEST`, `DS45PAD_VERSION` and its value `1.0.1-ps-usb-candidate[-debug]`, the `DS45_TOOLS` environment variable, `/* DS45PAD - ... */` banner comments (src/, shim/, patches/compat_pad.h), `tools/shim_pack.py`'s generated header comment, `tools/lv2scan.py`'s docstrings, everything under `docs/superpowers/{specs,plans,notes}/`, `patches/ps_only.patch`, the upstream credits "DS45Pad" in `README.md` and `CREDITS.md`, `tools/dumps/`, signing parameters, auth ids, magic check, shim-blob build.
- Passive only (project standard): no prompt, no toast, no retry for the fallback; it only leaves the trace marker.
- Nothing on the console is ever deleted; only `boot_plugins.txt` lines are edited.

### Rules for every executor (restated from the user's instructions)

- Build: `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (and `-Variant game`, each with and without `-Debug`). Host tests: `pwsh -File tests/run_tests.ps1`. Tools are PowerShell.
- Nothing Sony-proprietary in git: run `pwsh -File tools/check_no_proprietary.ps1` before every commit (exit 0 required).
- Commit messages: write the message with the Write tool to a file in your scratchpad (not in the repo), then `git commit -F <that file>`. Implementers end the message with the line `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- The Bash tool strips backslashes: do all file work with the Write/Edit tools, use the PowerShell tool for any shell command containing backslashes, and never create or modify files with heredocs.
- `rg` is not installed in the shell; use `git grep` (or the Grep tool) for searches.
- Implementers and reviewers never dispatch sub-agents.

## Review Focus

1. **An existing console with only the old files keeps loading its config.** Only `/dev_hdd0/plugins/ds45pad/ds45pad.txt` present -> every data path resolves under the legacy folder with the old names, so `ENABLE_SHIM = 1` is still read and the DS3 route survives the upgrade. Pinned by `test_legacy_config_selects_legacy_folder` (Task 1) and console step 4 (Task 7).
2. **Never two plugin entries in `boot_plugins.txt`.** Deploying over a console that lists the old debug path, the old release path, a commented old line, or already the new path (even twice) leaves exactly one plugin line, the new one; restore removes both names. Pinned by `tools/test_console_lib.ps1` cases (Task 4).
3. **The trace name the tools wait for is the one the plugin writes.** A mismatch makes deploy wait 3 min and report a failure though the plugin runs. Pinned by the self-test case that parses `DS45_BOOT_FILE` out of `src/ds45pad.h` and compares it with `$TraceFile` (Task 4).
4. **Calibration and kernel cache stay in one folder for the whole run.** A file read from one folder must never be written to the other, and code that runs without `ds45_paths_init()` (host tests linking `kernel.c`) must get the new-folder set, not garbage. Pinned by `test_resolution_happens_once` and `test_uninitialised_is_new_set` (Task 1).
5. **The game variant also resolves its config.** `paths.c` missing from one source list gives a link error or a game build that silently cannot load its config. Pinned by building all four variants and the string check on `nativepadps3_game_debug.sprx` (Task 3).

---

### Task 1: Data-folder resolver (`paths.c`), host tests and call sites

**Files:**
- Create: `src/paths.c`
- Create: `tests/test_paths.c`
- Modify: `src/ds45pad.h:35-41` (path macros), `src/kernel.h:31` (remove `DS45_KERNEL_CACHE_FILE`)
- Modify: `src/main.c:682-686` (init + config load), `src/usb.c:552`, `src/sticks.c:422,525`, `src/kernel.c:120,171,176`
- Modify: `src/Makefile:64-69` (`SRCS_VSH`, `SRCS_GAME`), `src/build.ps1:85` (`$sources`), `src/build.sh:51,57-59` (both `SRCS`)
- Modify tests that link `kernel.c` (add `paths.c` to their `/* SOURCES: */` line): `tests/test_btpresent.c`, `tests/test_lv2dump.c`, `tests/test_kernel.c`, `tests/test_scan.c`, `tests/test_passthru.c`, `tests/test_resolve.c`, `tests/test_lv2capture.c`; replace `DS45_KERNEL_CACHE_FILE` in `tests/test_kernel.c` and `tests/test_passthru.c` with `ds45_path(DS45_PATH_KERNEL_CACHE)`.

**Interfaces:**
- Consumes: `int ds45_file_exists(const char *path);` (util.c; weak fake in tests/fakes.c), `void ds45_boot_mark(const char *stage);` (util.c; weak fake).
- Produces (declared in `src/ds45pad.h`, replacing `DS45_DIR`, `DS45_CONFIG_FILE`, `DS45_DEVICE_FILE`, `DS45_CAL_FILE`; `DS45_DIAG_FILE` and `DS45_BOOT_FILE` stay as macros and are renamed in Task 2):
  ```c
  #define DS45_DATADIR_NEW    "/dev_hdd0/plugins/nativepadps3"
  #define DS45_DATADIR_LEGACY "/dev_hdd0/plugins/ds45pad"
  typedef enum {
      DS45_PATH_CONFIG = 0,
      DS45_PATH_DEVICES,
      DS45_PATH_CAL,
      DS45_PATH_KERNEL_CACHE,
      DS45_PATH_COUNT
  } ds45_path_kind_t;
  void        ds45_paths_init(void);
  const char *ds45_path(ds45_path_kind_t kind);
  ```
  Contract: `ds45_paths_init()` probes `DS45_DATADIR_NEW "/nativepadps3.txt"` then `DS45_DATADIR_LEGACY "/ds45pad.txt"` with `ds45_file_exists()`, selects the set (spec cases 1-3), and calls `ds45_boot_mark("datadir_new")` or `ds45_boot_mark("datadir_legacy")` exactly once. Each call re-probes (so tests can run several cases); the plugin calls it once. `ds45_path()` never probes; before any init it returns the new-folder set; an out-of-range kind returns `""` (an open of it fails harmlessly). The eight paths are compile-time string literals in two static tables (no allocation, no runtime concatenation).

- [ ] **Step 1: Write the failing test `tests/test_paths.c`**

First line `/* SOURCES: paths.c */`. Includes `fakes.h` and `ds45pad.h`. Overrides `ds45_boot_mark` and `ds45_boot_mark_hex` (the fakes are weak) to record marks into a local array with a count, cleared by a local `reset()` that also calls `fake_reset_all()`. Constants used in the assertions:

```c
#define NEW_CFG  "/dev_hdd0/plugins/nativepadps3/nativepadps3.txt"
#define NEW_DEV  "/dev_hdd0/plugins/nativepadps3/nativepadps3_devices.txt"
#define NEW_CAL  "/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt"
#define NEW_KC   "/dev_hdd0/plugins/nativepadps3/kernel_cache.txt"
#define OLD_CFG  "/dev_hdd0/plugins/ds45pad/ds45pad.txt"
#define OLD_DEV  "/dev_hdd0/plugins/ds45pad/ds45pad_devices.txt"
#define OLD_CAL  "/dev_hdd0/plugins/ds45pad/ds45pad_cal.txt"
#define OLD_KC   "/dev_hdd0/plugins/ds45pad/kernel_cache.txt"
```

A helper `static int is_new_set(void)` / `is_old_set(void)` returns 1 when all four `ds45_path()` values `strcmp`-equal the corresponding constants. Tests (each returns `__LINE__` on failure via `CHECK`, wired into `int run_tests(void)` like `tests/test_config.c`):

- `test_uninitialised_is_new_set`: run **first**, before any init: `CHECK(is_new_set())`; `CHECK(strcmp(ds45_path(DS45_PATH_COUNT), "") == 0)`; `CHECK(strcmp(ds45_path((ds45_path_kind_t)-1), "") == 0)`.
- `test_new_config_selects_new_folder`: `reset(); fake_file_set(NEW_CFG, "ENABLE = 1\n"); ds45_paths_init(); CHECK(is_new_set());` then again with both `NEW_CFG` and `OLD_CFG` present: `CHECK(is_new_set())`.
- `test_legacy_config_selects_legacy_folder`: only `OLD_CFG` present -> `CHECK(is_old_set())`. Also with `OLD_CFG` plus a stray `NEW_DEV` (devices file but no new config) -> still `is_old_set()` (the decision is made on the config file only).
- `test_no_config_defaults_to_new_folder`: no files -> `CHECK(is_new_set())`.
- `test_resolution_happens_once`: no files, init, then `fake_file_set(OLD_CFG, ...)` -> `CHECK(is_new_set())`; and the other direction: only `OLD_CFG`, init, then `fake_file_set(NEW_CFG, ...)` -> `CHECK(is_old_set())`.
- `test_trace_marker`: for each of the three cases, after `reset()` and one init: `CHECK(mark_count == 1)` and the mark is `"datadir_new"` (cases 1 and 3) or `"datadir_legacy"` (case 2).

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tests/run_tests.ps1 -Filter paths`
Expected: `FAIL test_paths:build` (`paths.c` does not exist / `ds45_path` undeclared).

- [ ] **Step 3: Implement `src/paths.c` and the header change**

`src/paths.c` starts with a banner `/* DS45PAD - data-folder resolution (new nativepadps3 folder, legacy ds45pad fallback). ... */` in the style of the other files (license line included), includes `ds45pad.h`, and implements `ds45_paths_init` / `ds45_path` per the Interfaces contract. In `src/ds45pad.h` replace lines 35-39 with the Produces block and a comment summarising the three resolution cases; keep `DS45_DIAG_FILE` and `DS45_BOOT_FILE` where they are. Delete `DS45_KERNEL_CACHE_FILE` from `src/kernel.h:31`.

- [ ] **Step 4: Run the new test to verify it passes**

Run: `pwsh -File tests/run_tests.ps1 -Filter paths`
Expected: no `FAIL` line, exit code 0.

- [ ] **Step 5: Move the call sites and wire the plugin**

- `src/main.c` `ds45_thread()`: call `ds45_paths_init();` right after `ds45_boot_mark(DS45PAD_VERSION);` and before `ds45_config_defaults(&g_cfg);` (this is the spec's "right after the start/version markers", ahead of `ds45_config_load`; the markers live in `ds45_thread`, not `ds45_start`, and `ds45_start` stays I/O-free). Then `ds45_config_load(&g_cfg, ds45_path(DS45_PATH_CONFIG));`.
- `src/usb.c:552`: `ds45_read_file(ds45_path(DS45_PATH_DEVICES), ...)`.
- `src/sticks.c:422` and `:525`: `ds45_path(DS45_PATH_CAL)`.
- `src/kernel.c:120,171,176`: `ds45_path(DS45_PATH_KERNEL_CACHE)`.
- Add `paths.c` to `SRCS_VSH` and `SRCS_GAME` in `src/Makefile`, `'paths'` to the shared `$sources` list in `src/build.ps1:85` (both variants), and `paths.c` to both `SRCS` strings in `src/build.sh`.
- Tests: add ` paths.c` to the SOURCES line of the seven tests listed under Files; in `tests/test_kernel.c` and `tests/test_passthru.c` replace every `DS45_KERNEL_CACHE_FILE` with `ds45_path(DS45_PATH_KERNEL_CACHE)` (these tests never call init, so they get the new-folder path).

- [ ] **Step 6: Run all host tests**

Run: `pwsh -File tests/run_tests.ps1`
Expected: no `FAIL` line, exit code 0.

Then: `git grep -n -E "DS45_(DIR|CONFIG_FILE|DEVICE_FILE|CAL_FILE|KERNEL_CACHE_FILE)\b" -- src tests`
Expected: no output.

- [ ] **Step 7: Build the debug VSH and the debug game variant (link check of the source lists)**

Run (PowerShell tool): `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` and `pwsh -File src/build.ps1 -Variant game -Debug -Sdk C:\usr\local\cell`
Expected: each ends with `Successfully built and signed ... SPRX` (output names are still `ds45pad_*` until Task 3).

- [ ] **Step 8: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0), then `git add src/paths.c src/ds45pad.h src/kernel.h src/main.c src/usb.c src/sticks.c src/kernel.c src/Makefile src/build.ps1 src/build.sh tests/` and `git commit -F <msg file>` with subject `paths: resolve the data folder once, nativepadps3 with ds45pad fallback`.

---

### Task 2: In-plugin identity strings

**Files:**
- Modify: `src/ds45pad.h:40-41` (`DS45_DIAG_FILE`, `DS45_BOOT_FILE`), `:13` area (historical-prefix note), `:322` (comment)
- Modify: `src/main.c:6,8,39,43,44,629,788,838,865,1018`
- Modify: `src/passthru.c:490`, `src/lv2dump.c:15,31-34,98`, `src/usb.c:32,584`, `src/diag.c:10`, `src/config.c:77`
- Test: `tests/test_lv2dump.c:11-14` and every `"ds45pad: "` assertion (lines 148, 189-195, 220, 240, 320, 354-357 and any other)

**Interfaces:**
- Consumes: Task 1's `ds45pad.h` layout.
- Produces: `DS45_BOOT_FILE "/dev_hdd0/tmp/nativepadps3_boot.txt"` (Task 4's self-test parses this exact `#define` line out of `src/ds45pad.h`: keep it on one line in the form `#define DS45_BOOT_FILE    "<path>"`), `DS45_DIAG_FILE "/dev_hdd0/tmp/nativepadps3_diag.txt"`.

- [ ] **Step 1: Update the test expectations first**

In `tests/test_lv2dump.c` set `A_BIN`/`A_TXT`/`B_BIN`/`B_TXT` to `/dev_hdd0/tmp/nativepadps3_lv2.bin`, `/dev_hdd0/tmp/nativepadps3_lv2.txt`, `/dev_hdd0/tmp/nativepadps3_lv2_b.bin`, `/dev_hdd0/tmp/nativepadps3_lv2_b.txt`, and replace every `"ds45pad: ` literal prefix with `"nativepadps3: `. Nothing else in the file changes.

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tests/run_tests.ps1 -Filter lv2dump`
Expected: a `FAIL` line from `test_lv2dump` (strings differ).

- [ ] **Step 3: Change the strings in the plugin**

Exactly the values in Global Constraints:
- `src/lv2dump.c` `DUMP_*` macros and the `put_str(..., "nativepadps3: ")` prefix; comment line 15 says `DUMP_LV2 = 1 in nativepadps3.txt`.
- `src/main.c`: `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0);`, `THREAD_NAME "nativepadps3"`, `STOP_THREAD_NAME "nppstop"`, `TOAST_BT_UNSUPPORTED "NativePadPS3: Bluetooth not supported on this kernel"`, `diag_msg("=== NativePadPS3 " DS45PAD_VERSION " ===");`, toasts `"NativePadPS3 Caricato / Loaded"` and `"NativePadPS3 Disattivato / Unloaded"`; header comment lines 6/8 name `nativepadps3_vsh.sprx` / `nativepadps3_game.sprx`; comment 788 names the config file as `nativepadps3.txt`.
- `src/passthru.c:490`: `"NativePadPS3 Bluetooth Attivo / Active"`. `src/usb.c:584`: `"NativePadPS3 device"`; comment 32 names `nativepadps3_devices.txt`.
- `src/ds45pad.h`: `DS45_DIAG_FILE`/`DS45_BOOT_FILE` per Produces; comment 322 and `src/config.c:77`, `src/diag.c:10`, `tests/test_config.c:12` name the new files.
- `src/ds45pad.h`, directly after the `#define DS45PAD_H` guard: a two-line comment stating that `ds45` / `DS45PAD` is the historical internal prefix kept for stability and that the console-facing name is NativePadPS3.

- [ ] **Step 4: Run all host tests**

Run: `pwsh -File tests/run_tests.ps1`
Expected: no `FAIL` line, exit code 0.

Then: `git grep -n -i ds45pad -- src/*.c src/*.h` and check every remaining hit is one of: `#include "ds45pad.h"` / a comment naming the header file `ds45pad.h`, `DS45PAD_H`, `DS45PAD_VERSION`, a `DS45PAD - ` banner, `DS45_DATADIR_LEGACY`'s definition, the historical-prefix note, or the legacy names in `src/paths.c`. Anything else is a miss: fix it.

- [ ] **Step 5: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0), `git add src tests/test_lv2dump.c tests/test_config.c`, commit with subject `plugin: console-visible name NativePadPS3 (module, threads, toasts, tmp files)`.

---

### Task 3: Build outputs and the four-variant build check

**Files:**
- Modify: `src/Makefile:1-11` (comments; banner line 1 stays), `:78`, `:104-105`, `:173-174`
- Modify: `src/build.ps1:117-120`
- Modify: `src/build.sh:10-13`, `:46`, `:55`, `:154-165` (banner line 2 stays)
- Modify: `patches/build_linux_wine.sh:32-36` (intermediates only; line 3 comment may say NativePadPS3; do not touch its stale source lists)

**Interfaces:**
- Consumes: Task 1 source lists (already include `paths.c`).
- Produces: `src/nativepadps3_vsh.sprx`, `src/nativepadps3_vsh_debug.sprx`, `src/nativepadps3_game.sprx`, `src/nativepadps3_game_debug.sprx`. Task 4's deploy default `-Sprx` is `../src/nativepadps3_vsh_debug.sprx`.

- [ ] **Step 1: Rename outputs and intermediates**

`PPU_PRX_TARGET = nativepadps3.prx`; `SPRX_VSH = nativepadps3_vsh$(DEBUG_SUFFIX).sprx`; `SPRX_GAME = nativepadps3_game$(DEBUG_SUFFIX).sprx`; `CLEANFILES` lists the four new names, then the four old `ds45pad_*` names (kept one cycle so `make clean` removes stale builds; add a comment saying so), then `$(PPU_PRX_TARGET)`. `build.ps1`: `nativepadps3.sym`, `nativepadps3.prx`, `nativepadps3.signed.tmp`, `"nativepadps3_$Variant$suffix.sprx"`. `build.sh`: `OUT="nativepadps3_game.sprx"` / `OUT="nativepadps3_vsh.sprx"`, `obj/nativepadps3.sym`, `obj/nativepadps3.prx`. Usage comments follow. Signing flags, auth ids, the magic check and the shim-blob steps are not touched.

- [ ] **Step 2: Build all four variants**

Run (PowerShell tool), one at a time:
```
pwsh -File src/build.ps1 -Variant vsh -Sdk C:\usr\local\cell
pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell
pwsh -File src/build.ps1 -Variant game -Sdk C:\usr\local\cell
pwsh -File src/build.ps1 -Variant game -Debug -Sdk C:\usr\local\cell
```
Expected: each prints `Successfully built and signed <variant> SPRX (...): ...\src\nativepadps3_<variant>[_debug].sprx` (build.ps1 throws `Invalid SCE container` if the magic is not `SCE\0`).

- [ ] **Step 3: String check on the binaries**

Run (PowerShell tool), for `src/nativepadps3_vsh_debug.sprx` and `src/nativepadps3_game_debug.sprx`:
```powershell
$s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('src/nativepadps3_vsh_debug.sprx'))
$hits = [regex]::Matches($s, '[\x20-\x7e]{6,}') | ForEach-Object Value | Where-Object { $_ -match '(?i)ds45pad|nativepadps3' }
$hits
$hits | Where-Object { $_ -match '(?i)ds45pad' -and $_ -notmatch '^/dev_hdd0/plugins/ds45pad/' }
```
Expected: the first list contains `/dev_hdd0/plugins/nativepadps3/nativepadps3.txt` and `/dev_hdd0/tmp/nativepadps3_boot.txt` (both binaries; the VSH debug one also `nativepadps3: ` and `/dev_hdd0/tmp/nativepadps3_lv2.bin`); the second command prints nothing (every old-name string is one of the four legacy data paths). If the first list is empty the SPRX was signed encrypted with scetool: rebuild that variant with `-Sign fself` for this check.

- [ ] **Step 4: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0; the `.sprx` files are git-ignored and must not be staged), `git add src/Makefile src/build.ps1 src/build.sh patches/build_linux_wine.sh`, commit with subject `build: emit nativepadps3_*.sprx`.

---

### Task 4: Console tools (offline only)

**Files:**
- Modify: `tools/ps3_console_lib.ps1` (constants, `Get-Trace`, boot-entry helpers)
- Modify: `tools/ps3_console_deploy.ps1`, `tools/ps3_console_restore.ps1`, `tools/ps3_console_test.ps1`, `tools/ps3_console_trace.ps1` (help text line 7)
- Create: `tools/test_console_lib.ps1`
- No change: `tools/ps3_console_linkkeys.ps1`

**Interfaces:**
- Consumes: `src/ds45pad.h` `#define DS45_BOOT_FILE "<path>"` (Task 2); build output `src/nativepadps3_vsh_debug.sprx` (Task 3).
- Produces (in `tools/ps3_console_lib.ps1`, defined before the functions, no network access at dot-source time):
  ```powershell
  $TraceFile          = '/dev_hdd0/tmp/nativepadps3_boot.txt'
  $RemoteDebugSprx    = '/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx'
  $ConfigFile         = '/dev_hdd0/plugins/nativepadps3/nativepadps3.txt'
  $LegacyConfigFile   = '/dev_hdd0/plugins/ds45pad/ds45pad.txt'
  $PluginEntryPattern = 'ds45pad|nativepadps3'
  function Set-BootPluginEntry([string[]]$Lines, [string]$Entry)       # -> string[]: blank lines and every line matching $PluginEntryPattern dropped, other lines kept verbatim in order, $Entry appended once
  function Remove-BootPluginEntries([string[]]$Lines)                   # -> string[]: blank lines and every line matching $PluginEntryPattern dropped
  function Test-BootPluginSingle([string[]]$Lines, [string]$Entry)     # -> bool: exactly one line matches $PluginEntryPattern and its Trim() equals $Entry
  ```
  Callers wrap results in `@(...)` (an empty result is `$null` in PowerShell). `Get-Trace` downloads `"$ftp$TraceFile"`.

- [ ] **Step 1: Write the failing self-test `tools/test_console_lib.ps1`**

Header comment: offline self-test, never contacts a console; run `pwsh -File tools/test_console_lib.ps1`. It sets `$Ip = 'offline.invalid'` (dot-sourcing only builds the `$ftp` string), dot-sources `ps3_console_lib.ps1`, and reports `PASS: <name>` / `FAIL: <name>` per case like `tools/test_check_no_proprietary.ps1`, exiting 1 on any failure. Fixtures (`$old = '/dev_hdd0/plugins/ds45pad_vsh_debug.sprx'`, `$oldRel = '/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx'`, `$web = '/dev_hdd0/plugins/webftp_server.sprx'`). Cases:

- `replace old debug entry`: `Set-BootPluginEntry @($web, $old) $RemoteDebugSprx` equals `@($web, $RemoteDebugSprx)`, and `Test-BootPluginSingle` on it is `$true`.
- `replace old release entry`: input `@($oldRel, $web)` -> `@($web, $RemoteDebugSprx)`.
- `old and new both listed, new twice`: input `@($old, $RemoteDebugSprx, $web, $RemoteDebugSprx)` -> `@($web, $RemoteDebugSprx)`.
- `commented old line and blank lines`: input `@('', "#$old", $web, '   ')` -> `@($web, $RemoteDebugSprx)`.
- `empty file`: input `@()` -> `@($RemoteDebugSprx)`.
- `single check rejects`: `Test-BootPluginSingle @($old) $RemoteDebugSprx` is `$false`; `@($RemoteDebugSprx, $RemoteDebugSprx)` is `$false`; `@($web)` is `$false`; `@("$RemoteDebugSprx  ")` is `$true`.
- `restore strips both names`: `Remove-BootPluginEntries @($old, $web, $RemoteDebugSprx, $oldRel)` equals `@($web)`; on `@($old)` the result is empty.
- `trace name matches the plugin`: read `src/ds45pad.h`, match `^#define\s+DS45_BOOT_FILE\s+"([^"]+)"`, and the captured path equals `$TraceFile`.
- `debug plugin name matches the build`: `Split-Path -Leaf $RemoteDebugSprx` equals `'nativepadps3_vsh_debug.sprx'`, and `src/build.ps1` contains the literal `"nativepadps3_$Variant$suffix.sprx"`.

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tools/test_console_lib.ps1`
Expected: `FAIL` lines (the helpers and constants do not exist), exit code 1.

- [ ] **Step 3: Implement the lib additions**

Add the Produces block to `tools/ps3_console_lib.ps1` and change `Get-Trace` to use `$TraceFile`. Lines match with PowerShell's default case-insensitive `-match`.

- [ ] **Step 4: Run the self-test**

Run: `pwsh -File tools/test_console_lib.ps1`
Expected: every case `PASS`, exit code 0.

- [ ] **Step 5: Rework `tools/ps3_console_deploy.ps1`**

- Default `-Sprx (Join-Path $PSScriptRoot '../src/nativepadps3_vsh_debug.sprx')`; `$remoteSprx = $RemoteDebugSprx`.
- Backup: download `$ConfigFile` to `prev_nativepadps3_$stamp.txt`; if that curl fails, download `$LegacyConfigFile` to `prev_ds45pad_$stamp.txt`; if both fail, `Say 'No config on the console (neither folder)'` and continue. `Say` which one was backed up (`backed up the new-folder config` / `backed up the legacy config`) and its contents, as today.
- Upload size check: `-match "\s$local\s.*$([regex]::Escape((Split-Path -Leaf $remoteSprx)))$"`.
- Config: write locally as `nativepadps3.txt` in `$Dest`, upload with `Curl-Ok @('--ftp-create-dirs', '-T', $cfg, "$ftp$ConfigFile")`.
- `boot_plugins.txt` step 3 becomes **replace**: `$cur = @(Get-Content $bp)`; if `-not (Test-BootPluginSingle $cur $remoteSprx)`, upload `@(Set-BootPluginEntry $cur $remoteSprx) -join "`n"`; then **always** re-download to `boot_plugins.check.txt` and `throw 'boot_plugins.txt must list exactly one plugin line, the new one'` unless `Test-BootPluginSingle (Get-Content $chk) $remoteSprx`; `Say` the result.
- Trace step 4: the listing match and the RNFR/RNTO use `$TraceFile` and `nativepadps3_boot_$stamp.txt`; old `ds45pad_boot*.txt` files are not touched.
- Help text (`.DESCRIPTION`) says it replaces any old or new plugin line with the new one, creates the config folder, and nothing on the console is deleted. Everything else (reboot, wait for `loop`, peeks, power-off sites) unchanged.

- [ ] **Step 6: Rework `tools/ps3_console_restore.ps1`, `ps3_console_test.ps1`, `ps3_console_trace.ps1`**

- restore: `$keep = @(Remove-BootPluginEntries (Get-Content $cur))`; the post-check throws `'a plugin line (ds45pad or nativepadps3) is still listed in boot_plugins.txt'` if any line of the re-read file matches `$PluginEntryPattern`. Help text: both names are removed; plugin files and both config folders stay on the disk.
- test.ps1 (does not dot-source the lib; keep it that way): defaults `../src/nativepadps3_vsh_debug.sprx` and `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx`, `$dumpFiles` and the `$done` keys use `nativepadps3_lv2.bin`, `nativepadps3_lv2.txt`, `nativepadps3_lv2_b.bin`, `nativepadps3_lv2_b.txt`, `$trace = 'nativepadps3_boot.txt'`, `$out = "nativepadps3_boot_btcheck_$stamp.txt"`, help line 11 `DUMP_LV2 = 1 in nativepadps3.txt`.
- trace.ps1: help line 7 names `/dev_hdd0/tmp/nativepadps3_boot.txt`.

- [ ] **Step 7: Offline checks**

Run: `pwsh -File tools/test_console_lib.ps1` -> all `PASS`, exit 0.
Run (parse-only, no execution): for each of the five `tools/ps3_console_*.ps1`: `$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path <file>), [ref]$null, [ref]$e); $e.Count` -> `0`.
Run: `git grep -n -i ds45pad -- tools` -> only `ps3_console_lib.ps1` (`$LegacyConfigFile`, `$PluginEntryPattern`), `ps3_console_deploy.ps1` (the `prev_ds45pad_$stamp.txt` legacy backup name), `test_console_lib.ps1` fixtures, `shim_pack.py:372`, `lv2scan.py` docstrings.
Do **not** run deploy/restore/test/trace/linkkeys against any address.

- [ ] **Step 8: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0), `git add tools/ps3_console_lib.ps1 tools/ps3_console_deploy.ps1 tools/ps3_console_restore.ps1 tools/ps3_console_test.ps1 tools/ps3_console_trace.ps1 tools/test_console_lib.ps1`, commit with subject `tools: nativepadps3 names; deploy replaces the boot entry, restore strips both`.

---

### Task 5: Shipped configs, `.gitignore`, README and user docs

**Files:**
- Move: `ps3_ready/plugins/ds45pad/ds45pad.txt` -> `ps3_ready/plugins/nativepadps3/nativepadps3.txt`; `ps3_ready/plugins/ds45pad/ds45pad_devices.txt` -> `ps3_ready/plugins/nativepadps3/nativepadps3_devices.txt` (`git mv`)
- Modify: `ps3_ready/boot_plugins.txt`, both moved files' header comments (lines 2-3)
- Modify: `.gitignore:11` (add `nativepadps3_boot*.txt` on the next line, keep `ds45pad_boot*.txt`)
- Modify: `README.md` lines 33, 71, 75, 77, 102, 118-124, 157, plus a new `### Upgrading from ds45pad` subsection at the end of "🚀 Installation (passthrough path)" (before "🛡️ Recovery & failsafe")
- Modify: `docs/BUILD.md`, `docs/COMPILAR.md`, `docs/INSTALAR_CFW.md`, `docs/INSTALAR_HEN.md`, `docs/PEDIR_BUILD.md`, `docs/DIAGNOSTICO.md`, `docs/ANALOGICOS.md`, `docs/ARQUITETURA.md`, `docs/TESTE_PS3XPAD.md`
- Do not touch: `README.md:9` and `:175`, `CREDITS.md:29` (upstream credits), `recovery/boot_plugins.txt`, anything under `docs/superpowers/`, `patches/ps_only.patch`

- [ ] **Step 1: `ps3_ready/` and `.gitignore`**

`git mv` both files; header line 2 of the config becomes `# NativePadPS3 - Configuration File`, of the devices file `# NativePadPS3 - Recognized USB Controllers`; line 3 of each names the new console path (`/dev_hdd0/plugins/nativepadps3/nativepadps3.txt`, `/dev_hdd0/plugins/nativepadps3/nativepadps3_devices.txt`). `ps3_ready/boot_plugins.txt` holds exactly `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`. `git status` must show the two files as renames.

- [ ] **Step 2: README**

Paths, file names, install steps, the `boot_plugins.txt` line and the build output (`src/nativepadps3_vsh.sprx`) per Global Constraints; install step 1 says to create `/dev_hdd0/plugins/nativepadps3/`. The upgrade subsection says, in the README's voice: the old `/dev_hdd0/plugins/ds45pad/` folder keeps working (the plugin falls back to it when the new folder has no `nativepadps3.txt`, and the boot trace shows `datadir_legacy`); to migrate, create `/dev_hdd0/plugins/nativepadps3/` and copy `ds45pad.txt` there as `nativepadps3.txt` (optionally `ds45pad_devices.txt` -> `nativepadps3_devices.txt` and `ds45pad_cal.txt` -> `nativepadps3_cal.txt`); from then on the new folder wins; in `boot_plugins.txt` **replace** the old plugin line with the new one, never add a second line.

- [ ] **Step 3: User docs**

Every hit of `git grep -n -i ds45pad -- ':(glob)docs/*.md'` changes per Global Constraints: `docs/BUILD.md` title `# Building NativePadPS3` and its paths/outputs (the `scetool ... -e ds45pad.prx ds45pad_vsh.sprx` example becomes `nativepadps3.prx nativepadps3_vsh.sprx`); `docs/COMPILAR.md` (including the stale `release/plugins/ds45pad/` -> `release/plugins/nativepadps3/`); `docs/INSTALAR_CFW.md`; `docs/INSTALAR_HEN.md` (also the stale `data/ds45pad.txt` reference -> the `ps3_ready/plugins/nativepadps3/nativepadps3.txt` file, and the stale toast text `DS45PAD 1.0.0 carregado` -> `NativePadPS3 Caricato / Loaded`); `docs/PEDIR_BUILD.md`; `docs/DIAGNOSTICO.md` (config name and `/dev_hdd0/tmp/nativepadps3_diag.txt`); `docs/ANALOGICOS.md` (cal file `/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt`); `docs/ARQUITETURA.md` (`nativepadps3_game.sprx`); `docs/TESTE_PS3XPAD.md:18` says to delete or comment out the NativePadPS3 line (older installs: the `ds45pad` line). These docs are in Portuguese; keep their language.

- [ ] **Step 4: Check**

Run: `git grep -n -i ds45pad -- ps3_ready README.md CREDITS.md ':(glob)docs/*.md'`
Expected: only `README.md` lines 9 and 175 (credits), the "Upgrading from ds45pad" subsection, `CREDITS.md:29`, and the "older installs" clause in `docs/TESTE_PS3XPAD.md`.

- [ ] **Step 5: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0), `git add -A ps3_ready .gitignore README.md docs/*.md`, commit with subject `docs, ps3_ready: NativePadPS3 names and the ds45pad upgrade path`.

---

### Task 6: Whole-branch verification gate (offline)

**Files:** none changed unless the gate finds a miss (fix it in the file that owns it and commit as `fix: leftover ds45pad reference in <file>`).

- [ ] **Step 1: Host tests and tool self-test**

Run: `pwsh -File tests/run_tests.ps1` -> no `FAIL`, exit 0. Run: `pwsh -File tools/test_console_lib.ps1` -> all `PASS`, exit 0. Run: `pwsh -File tools/test_check_no_proprietary.ps1` -> all `PASS`.

- [ ] **Step 2: All four builds plus the string check**

Repeat Task 3 Steps 2 and 3 on the final tree. Expected as there.

- [ ] **Step 3: Grep gate**

Run (PowerShell tool):
```powershell
git grep -n -i ds45pad -- src tools ps3_ready README.md CREDITS.md ':(glob)docs/*.md' |
  ForEach-Object { $p, $n, $t = $_ -split ':', 3; [pscustomobject]@{ Path = $p; Line = $n; Text = $t } } |
  Where-Object { $_.Text -notmatch '#include "ds45pad\.h"|ds45pad\.h\b|DS45PAD_H\b|DS45PAD_VERSION|DS45PAD - ' } |
  Format-Table -AutoSize -Wrap
```
Expected residue, and nothing else:
- `src/ds45pad.h`: the `DS45_DATADIR_LEGACY` definition and the historical-prefix note.
- `src/paths.c`: the legacy table (old file names) and its comments.
- `src/Makefile`: the old four names kept in `CLEANFILES` for one cycle (and its comment).
- `tools/ps3_console_lib.ps1`: `$LegacyConfigFile`, `$PluginEntryPattern`; `tools/ps3_console_deploy.ps1`: `prev_ds45pad_$stamp.txt`; `tools/test_console_lib.ps1`: fixtures; `tools/shim_pack.py:372`; `tools/lv2scan.py` docstrings.
- `README.md`: credits (lines 9, 175 or wherever they moved) and the "Upgrading from ds45pad" subsection; `CREDITS.md:29`; `docs/TESTE_PS3XPAD.md` "older installs" clause.

Also run `git grep -n -i ds45pad -- tests` and confirm the only hits are `#include "ds45pad.h"`, comments naming that header, and the legacy-path constants in `tests/test_paths.c`.

- [ ] **Step 4: Proprietary check and clean tree**

Run: `pwsh -File tools/check_no_proprietary.ps1` -> exit 0. Run: `git status --short` -> clean (untracked build outputs are ignored).

---

### Task 7: Console stage (controller runs it with the user; NOT delegated)

The controller does not dispatch a sub-agent for this task and never contacts the console itself: the **user** runs every command below on their machine and pastes the output; the controller reads it against the expectations. Hardware: DEX 4.92 test console. Results are kept under `tools/dumps/dex492/rename1/` (git-ignored).

**Console safety (binding for every step):**
- No peek bursts: only the tools' throttled peeks; never a hand-written peek loop.
- Arm DualSenses by BD address only (`ps3_console_linkkeys.ps1 -Arm -Bd <address>`), never the Move motion controller's address; addresses are never written into the repo.
- Every console session ends with `tools/ps3_console_restore.ps1`, even after a failure.
- If the console hangs: hold the physical power button. Never hard reset.

- [ ] **Step 1: Before state.** User confirms `boot_plugins.txt` lists `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx`, the old folder holds the live config, and `/dev_hdd0/plugins/nativepadps3/` does not exist (an FTP listing of `/dev_hdd0/plugins/`).

- [ ] **Step 2: Deploy the new name.** User runs `pwsh tools/ps3_console_deploy.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1` (default config). Expected: "backed up the legacy config"; config uploaded (folder created); `boot_plugins.txt now:` shows exactly one plugin line, `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx`; reboot; "Plugin in its loop".

- [ ] **Step 3: Trace check, new folder.** User runs `Select-String -Path tools/dumps/dex492/rename1/boot.txt -Pattern '^start|^1\.0\.1|datadir_|config_bytes|config_NOT_FOUND|kshim|^loop'`. Expected in order: `start`, the version, `datadir_new`, `config_bytes=...`, the `kshim` lines as before the rename, `loop`; no `config_NOT_FOUND_using_defaults`. Then (pads OFF) `pwsh tools/ps3_console_linkkeys.ps1 -Ip <console> -Arm -Bd <DualSense address(es)>`, pads on, and the user confirms the DualSense behaves exactly as before (DS3 route, rumble, power-off); `pwsh tools/ps3_console_trace.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1` shows `shimdbg` lines.

- [ ] **Step 4: Legacy fallback check.** User renames the new config out of the way and stamps the trace, then reboots: `curl.exe -Q "RNFR /dev_hdd0/plugins/nativepadps3/nativepadps3.txt" -Q "RNTO /dev_hdd0/plugins/nativepadps3/nativepadps3.txt.off" ftp://<console>/`, the same RNFR/RNTO for `/dev_hdd0/tmp/nativepadps3_boot.txt` -> `nativepadps3_boot_legacy-before.txt`, then `curl.exe http://<console>/reboot.ps3`. After it is back: `pwsh tools/ps3_console_trace.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1` and `Select-String -Path tools/dumps/dex492/rename1/boot_now.txt -Pattern 'datadir_|config_bytes|config_NOT_FOUND'`. Expected: `datadir_legacy`, `config_bytes` matching the old `ds45pad.txt`, no `config_NOT_FOUND`; arm by BD address again (the reboot reset it) and the pad behaves the same. Then the user renames `nativepadps3.txt.off` back to `nativepadps3.txt`, stamps the trace again, reboots, and the trace shows `datadir_new` (both are valid end states; this proves the new folder wins again).

- [ ] **Step 5: Restore.** User runs `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`. Expected: `after:` shows no line with `ds45pad` or `nativepadps3`; reboot; event slot stock; `CLEAN: no shim installed, the hooks are stock.` The stage ends here with the console stock.

- [ ] **Step 6: Record.** The controller writes `docs/superpowers/notes/2026-11-08-nativepadps3-rename-console.md` (what was run, the trace markers seen, the pad behaviour, any deviation; no BD addresses, no dumps) and updates the spec's `Status:` line to say the console stage passed (or what failed). `pwsh -File tools/check_no_proprietary.ps1` (exit 0), commit both with `git commit -F`.
