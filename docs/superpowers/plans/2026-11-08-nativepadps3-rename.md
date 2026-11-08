# NativePadPS3 Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (the user's standing choice of execution method; do not ask, never execute inline with executing-plans) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The plugin builds, deploys, configures and traces itself as `nativepadps3` (step 1), and then every remaining internal `ds45` identifier, file name and banner becomes `npp` (step 2), while a console that only has the old `/dev_hdd0/plugins/ds45pad/` config folder keeps working unchanged.

**Architecture:** Step 1 (Tasks 1-7) is hand-written: a new pure module `src/paths.c` resolves the data folder once at plugin start (new if its config exists, else legacy if its config exists, else new) and hands out the four data-file paths; console-visible strings, build outputs, tools, `ps3_ready/` and docs switch to the new names, and deploy *replaces* the `boot_plugins.txt` entry. Step 2 (Tasks 8-12) is a scripted, reproducible substring rename `ds45` -> `npp` driven by an ordered table, an explicit file list and exact-line exclusions, followed by a few hand-written lockstep hardenings and gates, the strictest being a byte-identical shim blob.

**Tech Stack:** C (Cell SDK PPU gcc for the plugin, LLVM clang for host tests and the shim blob), Python (shim packer, lv2scan, their pytest suites), PowerShell 7 (build script, console tools, rename script, self-tests), make/bash (alternate build scripts).

**Spec:** [docs/superpowers/specs/2026-11-08-nativepadps3-rename-design.md](../specs/2026-11-08-nativepadps3-rename-design.md) (approved, including Amendment 1 and the confirmed decisions (a)-(d) at its end; binding). Executors read the spec and this plan. Identifiers in Tasks 1-8 are in their pre-rename form; Tasks 9-12 use the post-rename form. Inventory: `.superpowers/rename-brainstorm-1.md` (git-ignored scratch).

## Global Constraints

- Branch `feat/nativepad-rename`. Do not switch branches, do not push. Never contact the console 192.168.1.97 or any other network device (Tasks 1-6 and 8-11 are fully offline; Tasks 7 and 12 are run by the controller with the user).
- Naming (Q1): all-lowercase `nativepadps3` for on-console file names, paths, TTY prefix and the main thread; display strings use `NativePadPS3`.
- Build outputs: `nativepadps3_vsh.sprx`, `nativepadps3_vsh_debug.sprx`, `nativepadps3_game.sprx`, `nativepadps3_game_debug.sprx`. Intermediates: `nativepadps3.sym`, `nativepadps3.prx`, `nativepadps3.signed.tmp`.
- Release plugin on console: `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`. Debug plugin (deploy tool): `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx` (plugins root, as today).
- Data folder `/dev_hdd0/plugins/nativepadps3/`: config `nativepadps3.txt`, USB device list `nativepadps3_devices.txt`, stick calibration `nativepadps3_cal.txt`, kernel match cache `kernel_cache.txt` (unchanged name, inside whichever data folder is resolved; its first-line magic `DS45K1` is unchanged).
- Legacy data folder `/dev_hdd0/plugins/ds45pad/` with the **old** file names `ds45pad.txt`, `ds45pad_devices.txt`, `ds45pad_cal.txt`, `kernel_cache.txt`. These literals live in `src/paths.c`, each under the comment `legacy on-console name, read-only compatibility, do not rename`, on lines that carry no other `ds45` token.
- Boot trace `/dev_hdd0/tmp/nativepadps3_boot.txt`; diag log `/dev_hdd0/tmp/nativepadps3_diag.txt`; LV2 dumps (debug VSH only) `/dev_hdd0/tmp/nativepadps3_lv2.bin/.txt`, `nativepadps3_lv2_b.bin/.txt`. Always the new names, whichever data folder is resolved.
- PRX module name `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0)`. Threads `"nativepadps3"` and `"nppstop"` (decision (d)).
- Toasts: `NativePadPS3 Caricato / Loaded`, `NativePadPS3 Disattivato / Unloaded`, `NativePadPS3 Bluetooth Attivo / Active`, `NativePadPS3: Bluetooth not supported on this kernel`. Diag header `=== NativePadPS3 <version> ===`. TTY prefix `nativepadps3: `. USB default device name `NativePadPS3 device`. Linkkeys webMAN popup `NativePadPS3: power the pads on now`.
- Trace markers `datadir_new` / `datadir_legacy`; `config_NOT_FOUND_using_defaults` stays.
- Release boot entry (`ps3_ready/boot_plugins.txt`): `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`.
- Tools: `$PluginEntryPattern = 'ds45pad|nativepadps3'` in `tools/ps3_console_lib.ps1` (a legacy on-console name, labelled like the `paths.c` literals).
- Version string value `1.0.1-ps-usb-candidate[-debug]`, signing parameters, auth ids, magic check, every kernel address/offset: unchanged.
- Step 2 rename: `DS45PAD_<X>` -> `NPP_<X>`, `DS45PAD` -> `NPP`, `DS45Pad`/`Ds45Pad` -> `NativePadPS3`, `ds45pad_<x>` -> `npp_<x>`, `ds45pad` -> `npp`, `DS45_<X>` -> `NPP_<X>`, `ds45_<x>` -> `npp_<x>`, bare `ds45` -> `npp`. Files: `src/ds45pad.h` -> `src/npp.h`, `src/ds45_state.h` -> `src/npp_state.h` (`git mv` in the same commit as the content change). `DS45_TOOLS` -> `NPP_TOOLS`.
- Bytes that stay (decision (a)): `NPP_SHIM_MAGIC = 0x4453343553484D31ULL /* "DS45SHM1" */` and `CACHE_MAGIC "DS45K1"`, plus the `"DS45SHM1"` literal in `tests/test_ds3report.c` and the `DS45K1`/`DS45K0`/`ds45k1` literals in `tests/test_kernel.c`, and the legacy on-console names.
- `NPP_TOOLS` (decision (b)): for one cycle `build.ps1` and `build.sh` fail loudly with a clear message if `DS45_TOOLS` is set and `NPP_TOOLS` is not.
- Never edited (decision (c)): `docs/superpowers/**` history (all older specs, plans, notes, and this plan), `patches/ps_only.patch`, `CREDITS.md`, `LICENSE`, the upstream-credit lines `README.md` 9 and 175, `tools/dumps/`. The spec itself only gets its Status line and the confirmation section.
- Passive only (project standard): no prompt, no toast, no retry for the fallback; it only leaves the trace marker. Nothing on the console is ever deleted; only `boot_plugins.txt` lines are edited.

### Rules for every executor (restated from the user's instructions)

- Build: `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (and `-Variant game`, each with and without `-Debug`). Host tests: `pwsh -File tests/run_tests.ps1`. Tools are PowerShell. After Task 10 the signing-tools variable is `NPP_TOOLS`.
- Nothing Sony-proprietary in git: run `pwsh -File tools/check_no_proprietary.ps1` before every commit (exit 0 required).
- Commit messages: write the message with the Write tool to a file in your scratchpad (not in the repo), then `git commit -F <that file>`. Implementers end the message with the line `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- The Bash tool strips backslashes: do all file work with the Write/Edit tools, use the PowerShell tool for any shell command containing backslashes, and never create or modify files with heredocs.
- `rg` is not installed in the shell; use `git grep` (or the Grep tool). In `git grep` pathspecs write `':(glob)docs/*.md'` so `docs/superpowers/` is not matched.
- Step-2 working files go in `.superpowers/rename/` (git-ignored through `.git/info/exclude`, shared by every agent on this machine, never committed).
- Implementers and reviewers never dispatch sub-agents.

## Review Focus

1. **An existing console with only the old files keeps loading its config.** Only `/dev_hdd0/plugins/ds45pad/ds45pad.txt` present -> every data path resolves under the legacy folder with the old names, so `ENABLE_SHIM = 1` is still read and the DS3 route survives the upgrade. Pinned by `test_legacy_config_selects_legacy_folder` (Task 1), the rename script's exclusion check (Task 8) and console step A4 (Task 7).
2. **Never two plugin entries in `boot_plugins.txt`.** Deploying over a console that lists the old debug path, the old release path, a commented old line, or the new path twice leaves exactly one plugin line, the new one; restore removes both names. Pinned by `tools/test_console_lib.ps1` (Task 4); it must still pass after step 2 (Task 11).
3. **The tools find what the plugin writes.** The trace path the tools wait for equals `DS45_BOOT_FILE` (Task 4 self-test), and the power-off offsets the tools read come from the generated header, not a silent hard-coded fallback, whatever the prefix (Task 10 `Get-PoOffsets` cases).
4. **Bytes on the console do not move.** The shim blob is byte-identical before and after step 2, `DS45SHM1` and `DS45K1` keep their bytes, so a blob resident from an earlier load is still adopted and an old `kernel_cache.txt` still parses. Pinned by the blob-identity gate (Tasks 8-9), `tests/test_ds3report.c` and `tests/test_kernel.c`.
5. **A missed rename in a name that is looked up as text.** Packer symbol `npp_shim_data` (incl. the inline-asm literals in `shim/entry.c`), the generated header names consumed by `kshim.c`/`test_kshim.c`/`test_shim_pack.py`, lv2scan's emitted identifiers, the `/* CFLAGS: -DNPP_DEBUG */` lines and `-DNPP_HOST_TEST`. Pinned by the rename script's residue report (Task 8), the full host/Python suites and four builds (Task 9), the grep gate (Task 11) and console stage B (Task 12).

---

## Step 1 - functional rename (pre-rename identifiers)

### Task 1: Data-folder resolver (`paths.c`), host tests and call sites

**Files:**
- Create: `src/paths.c`, `tests/test_paths.c`
- Modify: `src/ds45pad.h:35-39` (path macros), `src/kernel.h:31` (remove `DS45_KERNEL_CACHE_FILE`)
- Modify: `src/main.c:682-686`, `src/usb.c:552`, `src/sticks.c:422,525`, `src/kernel.c:120,171,176`
- Modify: `src/Makefile:64-69` (`SRCS_VSH`, `SRCS_GAME`), `src/build.ps1:85` (`$sources`), `src/build.sh:51,57-59` (both `SRCS`)
- Modify (add `paths.c` to the `/* SOURCES: */` line): `tests/test_btpresent.c`, `tests/test_lv2dump.c`, `tests/test_kernel.c`, `tests/test_scan.c`, `tests/test_passthru.c`, `tests/test_resolve.c`, `tests/test_lv2capture.c` (all link `kernel.c`); replace `DS45_KERNEL_CACHE_FILE` in `tests/test_kernel.c` and `tests/test_passthru.c` with `ds45_path(DS45_PATH_KERNEL_CACHE)`.

**Interfaces:**
- Consumes: `int ds45_file_exists(const char *path);` and `void ds45_boot_mark(const char *stage);` (util.c; weak fakes in `tests/fakes.c`).
- Produces (declared in `src/ds45pad.h`, replacing `DS45_DIR`, `DS45_CONFIG_FILE`, `DS45_DEVICE_FILE`, `DS45_CAL_FILE`; `DS45_DIAG_FILE`/`DS45_BOOT_FILE` stay and are renamed in Task 2):
  ```c
  #define DS45_DATADIR_NEW "/dev_hdd0/plugins/nativepadps3"
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
  Contract: `ds45_paths_init()` probes `DS45_DATADIR_NEW "/nativepadps3.txt"`, then the legacy config, with `ds45_file_exists()`, selects the set (spec cases 1-3) and calls `ds45_boot_mark("datadir_new")` or `ds45_boot_mark("datadir_legacy")` exactly once. Each call re-probes (tests run several cases); the plugin calls it once. `ds45_path()` never probes; before any init it returns the new-folder set; an out-of-range kind returns `""`. The eight paths are compile-time literals in two static tables (no allocation). The legacy folder is **not** in the header: `paths.c` defines it locally, with a macro name that has no `ds45` in it, exactly:
  ```c
  /* legacy on-console name, read-only compatibility, do not rename */
  #define LEGACY_DIR "/dev_hdd0/plugins/ds45pad"
  ```
  and each legacy table entry (`LEGACY_DIR "/ds45pad.txt"`, `LEGACY_DIR "/ds45pad_devices.txt"`, `LEGACY_DIR "/ds45pad_cal.txt"`, `LEGACY_DIR "/kernel_cache.txt"`) sits on its own line under the same comment, with no other `ds45` token on that line (Task 8 excludes these lines by exact content).

- [ ] **Step 1: Write the failing test `tests/test_paths.c`**

First line `/* SOURCES: paths.c */`. Includes `fakes.h` and `ds45pad.h`. Overrides `ds45_boot_mark` and `ds45_boot_mark_hex` (the fakes are weak) to record marks in a local array with a count; a local `reset()` clears it and calls `fake_reset_all()`. Constants (the four `OLD_*` lines carry the legacy label comment above them):

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

Helpers `is_new_set()` / `is_old_set()` return 1 when all four `ds45_path()` values `strcmp`-equal the matching constants. Tests (return `__LINE__` via `CHECK`, wired into `int run_tests(void)` as in `tests/test_config.c`):

- `test_uninitialised_is_new_set` (runs **first**): `CHECK(is_new_set())`; `CHECK(strcmp(ds45_path(DS45_PATH_COUNT), "") == 0)`; `CHECK(strcmp(ds45_path((ds45_path_kind_t)-1), "") == 0)`.
- `test_new_config_selects_new_folder`: only `NEW_CFG` -> `is_new_set()`; both `NEW_CFG` and `OLD_CFG` -> `is_new_set()`.
- `test_legacy_config_selects_legacy_folder`: only `OLD_CFG` -> `is_old_set()`; `OLD_CFG` plus a stray `NEW_DEV` -> still `is_old_set()`.
- `test_no_config_defaults_to_new_folder`: no files -> `is_new_set()`.
- `test_resolution_happens_once`: no files, init, then `fake_file_set(OLD_CFG, ...)` -> `is_new_set()`; only `OLD_CFG`, init, then `fake_file_set(NEW_CFG, ...)` -> `is_old_set()`.
- `test_trace_marker`: for each of the three cases, after `reset()` and one init: `CHECK(mark_count == 1)` and the mark is `"datadir_new"` (cases 1, 3) or `"datadir_legacy"` (case 2).

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tests/run_tests.ps1 -Filter paths`
Expected: `FAIL test_paths:build`.

- [ ] **Step 3: Implement `src/paths.c` and the header change**

`src/paths.c` opens with a banner in the style of the other files (`/* DS45PAD - data-folder resolution ... Licensed under GPLv2 - see LICENSE. */`; Task 9 turns it into `NPP - ...`), includes `ds45pad.h`, and implements the Interfaces contract. In `src/ds45pad.h` replace lines 35-39 with the Produces block and a comment summarising the three resolution cases. Delete `DS45_KERNEL_CACHE_FILE` from `src/kernel.h:31`.

- [ ] **Step 4: Run the new test to verify it passes**

Run: `pwsh -File tests/run_tests.ps1 -Filter paths` -> no `FAIL`, exit 0.

- [ ] **Step 5: Move the call sites and wire the plugin**

- `src/main.c` `ds45_thread()`: `ds45_paths_init();` right after `ds45_boot_mark(DS45PAD_VERSION);`, before `ds45_config_defaults(&g_cfg);` (the spec's "right after the start/version markers", ahead of `ds45_config_load`; the markers are in `ds45_thread`, and `ds45_start` stays I/O-free); then `ds45_config_load(&g_cfg, ds45_path(DS45_PATH_CONFIG));`.
- `src/usb.c:552` -> `ds45_path(DS45_PATH_DEVICES)`; `src/sticks.c:422,525` -> `ds45_path(DS45_PATH_CAL)`; `src/kernel.c:120,171,176` -> `ds45_path(DS45_PATH_KERNEL_CACHE)`.
- Add `paths.c` to `SRCS_VSH` and `SRCS_GAME` in `src/Makefile`, `'paths'` to the shared `$sources` in `src/build.ps1:85`, `paths.c` to both `SRCS` strings in `src/build.sh`.
- Tests: the SOURCES lines and the `DS45_KERNEL_CACHE_FILE` replacements listed under Files.

- [ ] **Step 6: Run all host tests**

Run: `pwsh -File tests/run_tests.ps1` -> no `FAIL`, exit 0.
Run: `git grep -n -E "DS45_(DIR|CONFIG_FILE|DEVICE_FILE|CAL_FILE|KERNEL_CACHE_FILE|DATADIR_LEGACY)\b" -- src tests` -> no output.

- [ ] **Step 7: Link check of both variants**

Run (PowerShell tool): `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` and `pwsh -File src/build.ps1 -Variant game -Debug -Sdk C:\usr\local\cell` -> each ends `Successfully built and signed ... SPRX` (names still `ds45pad_*` until Task 3).

- [ ] **Step 8: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add src/paths.c src/ds45pad.h src/kernel.h src/main.c src/usb.c src/sticks.c src/kernel.c src/Makefile src/build.ps1 src/build.sh tests/`; subject `paths: resolve the data folder once, nativepadps3 with ds45pad fallback`.

---

### Task 2: In-plugin identity strings

**Files:**
- Modify: `src/ds45pad.h:40-41` (`DS45_DIAG_FILE`, `DS45_BOOT_FILE`), `:322` (comment)
- Modify: `src/main.c:6,8,39,43,44,629,788,838,865,1018`; `src/passthru.c:490`; `src/lv2dump.c:15,31-34,98`; `src/usb.c:32,584`; `src/diag.c:10`; `src/config.c:77`; `tests/test_config.c:12` (comment)
- Test: `tests/test_lv2dump.c:11-14` and every `"ds45pad: "` assertion (148, 189-195, 220, 240, 320, 354-357 and any other)

**Interfaces:**
- Consumes: Task 1's `ds45pad.h` layout.
- Produces: `#define DS45_BOOT_FILE    "/dev_hdd0/tmp/nativepadps3_boot.txt"` on one line in exactly that shape (Task 4's self-test parses `^#define\s+DS45_BOOT_FILE\s+"([^"]+)"`; Task 9 renames the macro and the self-test with it), `DS45_DIAG_FILE "/dev_hdd0/tmp/nativepadps3_diag.txt"`.

- [ ] **Step 1: Update the test expectations first**

In `tests/test_lv2dump.c`: `A_BIN`/`A_TXT`/`B_BIN`/`B_TXT` = `/dev_hdd0/tmp/nativepadps3_lv2.bin`, `.../nativepadps3_lv2.txt`, `.../nativepadps3_lv2_b.bin`, `.../nativepadps3_lv2_b.txt`; every `"ds45pad: ` literal prefix -> `"nativepadps3: `. Nothing else changes.

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tests/run_tests.ps1 -Filter lv2dump` -> a `FAIL` line from `test_lv2dump`.

- [ ] **Step 3: Change the strings**

Exactly the Global Constraints values: `src/lv2dump.c` `DUMP_*` macros, the `"nativepadps3: "` prefix and comment 15 (`DUMP_LV2 = 1 in nativepadps3.txt`); `src/main.c` `SYS_MODULE_INFO(NATIVEPADPS3, 0, 1, 0);`, `THREAD_NAME "nativepadps3"`, `STOP_THREAD_NAME "nppstop"`, `TOAST_BT_UNSUPPORTED "NativePadPS3: Bluetooth not supported on this kernel"`, `diag_msg("=== NativePadPS3 " DS45PAD_VERSION " ===");`, the two toasts, header comments 6/8 (`nativepadps3_vsh.sprx` / `nativepadps3_game.sprx`), comment 788 (`nativepadps3.txt`); `src/passthru.c:490` `"NativePadPS3 Bluetooth Attivo / Active"`; `src/usb.c:584` `"NativePadPS3 device"`, comment 32 `nativepadps3_devices.txt`; `src/ds45pad.h` `DS45_DIAG_FILE`/`DS45_BOOT_FILE` per Produces; comments `src/ds45pad.h:322`, `src/config.c:77`, `src/diag.c:10`, `tests/test_config.c:12` name the new files. No "historical prefix" note (Q4 reversed).

- [ ] **Step 4: Run all host tests and the leftover check**

Run: `pwsh -File tests/run_tests.ps1` -> no `FAIL`, exit 0.
Run: `git grep -n -i -E "ds45pad[._: ]|DS45Pad" -- src/*.c src/*.h tests/*.c` -> only `ds45pad.h` file-name mentions (includes, "see ds45pad.h"), `DS45PAD - ` banners, and the labelled legacy lines in `src/paths.c` and `tests/test_paths.c`.

- [ ] **Step 5: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add src tests/test_lv2dump.c tests/test_config.c`; subject `plugin: console-visible name NativePadPS3 (module, threads, toasts, tmp files)`.

---

### Task 3: Build outputs and the four-variant build check

**Files:**
- Modify: `src/Makefile:6-11` (comments), `:78`, `:104-105`, `:173-174`
- Modify: `src/build.ps1:117-120`
- Modify: `src/build.sh:10-13`, `:46`, `:55`, `:154-165`
- Modify: `patches/build_linux_wine.sh:32-36` (intermediates only; do not touch its stale source lists)

**Interfaces:**
- Consumes: Task 1 source lists.
- Produces: `src/nativepadps3_vsh.sprx`, `src/nativepadps3_vsh_debug.sprx`, `src/nativepadps3_game.sprx`, `src/nativepadps3_game_debug.sprx`; `build.ps1` contains the literal `"nativepadps3_$Variant$suffix.sprx"` (Task 4 self-test checks it).

- [ ] **Step 1: Rename outputs and intermediates**

`PPU_PRX_TARGET = nativepadps3.prx`; `SPRX_VSH = nativepadps3_vsh$(DEBUG_SUFFIX).sprx`; `SPRX_GAME = nativepadps3_game$(DEBUG_SUFFIX).sprx`; `CLEANFILES` = the four new names, then `$(PPU_PRX_TARGET)`. Do **not** keep the old four `ds45pad_*` names in `CLEANFILES` (the spec allows it, but step 2 would rename them to `npp_*` and the line would mean nothing; stale local builds are git-ignored). `build.ps1`: `nativepadps3.sym`, `nativepadps3.prx`, `nativepadps3.signed.tmp`, `"nativepadps3_$Variant$suffix.sprx"`. `build.sh`: `OUT="nativepadps3_game.sprx"` / `OUT="nativepadps3_vsh.sprx"`, `obj/nativepadps3.sym`, `obj/nativepadps3.prx`. Usage comments follow. Signing flags, auth ids, magic check, shim-blob steps untouched.

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

Run (PowerShell tool) for `src/nativepadps3_vsh_debug.sprx` and `src/nativepadps3_game_debug.sprx`:
```powershell
$s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes('src/nativepadps3_vsh_debug.sprx'))
$hits = [regex]::Matches($s, '[\x20-\x7e]{6,}') | ForEach-Object Value | Where-Object { $_ -match '(?i)ds45|nativepadps3' }
$hits
$hits | Where-Object { $_ -match '(?i)ds45' -and $_ -notmatch '^/dev_hdd0/plugins/ds45pad/' -and $_ -notmatch 'DS45SHM1|DS45K1' }
```
Expected: the first list contains `/dev_hdd0/plugins/nativepadps3/nativepadps3.txt` and `/dev_hdd0/tmp/nativepadps3_boot.txt` (both; VSH debug also `nativepadps3: `); the second prints nothing. If the first list is empty the SPRX is encrypted (scetool): rebuild that variant with `-Sign fself` for this check. Keep this snippet; Tasks 6 and 9 rerun it.

- [ ] **Step 4: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0; `.sprx` files are git-ignored); `git add src/Makefile src/build.ps1 src/build.sh patches/build_linux_wine.sh`; subject `build: emit nativepadps3_*.sprx`.

---

### Task 4: Console tools (offline only)

**Files:**
- Modify: `tools/ps3_console_lib.ps1`, `tools/ps3_console_deploy.ps1`, `tools/ps3_console_restore.ps1`, `tools/ps3_console_test.ps1`, `tools/ps3_console_trace.ps1` (help line 7), `tools/ps3_console_linkkeys.ps1:62` (popup text)
- Create: `tools/test_console_lib.ps1`

**Interfaces:**
- Consumes: `src/ds45pad.h` `DS45_BOOT_FILE` line (Task 2); `src/build.ps1` output literal (Task 3).
- Produces (in `tools/ps3_console_lib.ps1`, before the functions; no network access at dot-source time):
  ```powershell
  $TraceFile          = '/dev_hdd0/tmp/nativepadps3_boot.txt'
  $RemoteDebugSprx    = '/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx'
  $ConfigFile         = '/dev_hdd0/plugins/nativepadps3/nativepadps3.txt'
  # legacy on-console name, read-only compatibility, do not rename
  $LegacyConfigFile   = '/dev_hdd0/plugins/ds45pad/ds45pad.txt'
  # legacy on-console name, read-only compatibility, do not rename
  $PluginEntryPattern = 'ds45pad|nativepadps3'
  function Set-BootPluginEntry([string[]]$Lines, [string]$Entry)    # -> string[]: blank lines and every line matching $PluginEntryPattern dropped, other lines kept verbatim in order, $Entry appended once
  function Remove-BootPluginEntries([string[]]$Lines)                # -> string[]: blank lines and every line matching $PluginEntryPattern dropped
  function Test-BootPluginSingle([string[]]$Lines, [string]$Entry)  # -> bool: exactly one line matches $PluginEntryPattern and its Trim() equals $Entry
  ```
  Callers wrap results in `@(...)`. `Get-Trace` downloads `"$ftp$TraceFile"`. These are the only `ds45` lines in `tools/` besides the self-test fixtures (Task 8 excludes them by exact content).

- [ ] **Step 1: Write the failing self-test `tools/test_console_lib.ps1`**

Header comment: offline self-test, never contacts a console; run `pwsh -File tools/test_console_lib.ps1`. Sets `$Ip = 'offline.invalid'` (dot-sourcing only builds the `$ftp` string), dot-sources the lib, prints `PASS: <name>` / `FAIL: <name>` per case like `tools/test_check_no_proprietary.ps1`, exits 1 on any failure. Fixtures, each on its own line under the legacy label comment: `$old = '/dev_hdd0/plugins/ds45pad_vsh_debug.sprx'`, `$oldRel = '/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx'`; plus `$web = '/dev_hdd0/plugins/webftp_server.sprx'`. Cases:

- `replace old debug entry`: `Set-BootPluginEntry @($web, $old) $RemoteDebugSprx` equals `@($web, $RemoteDebugSprx)`; `Test-BootPluginSingle` on it is `$true`.
- `replace old release entry`: `@($oldRel, $web)` -> `@($web, $RemoteDebugSprx)`.
- `old and new both listed, new twice`: `@($old, $RemoteDebugSprx, $web, $RemoteDebugSprx)` -> `@($web, $RemoteDebugSprx)`.
- `commented old line and blank lines`: `@('', "#$old", $web, '   ')` -> `@($web, $RemoteDebugSprx)`.
- `empty file`: `@()` -> `@($RemoteDebugSprx)`.
- `single check rejects`: `@($old)` -> `$false`; `@($RemoteDebugSprx, $RemoteDebugSprx)` -> `$false`; `@($web)` -> `$false`; `@("$RemoteDebugSprx  ")` -> `$true`.
- `restore strips both names`: `Remove-BootPluginEntries @($old, $web, $RemoteDebugSprx, $oldRel)` equals `@($web)`; on `@($old)` the result is empty.
- `trace name matches the plugin`: read `src/ds45pad.h`, match `^#define\s+DS45_BOOT_FILE\s+"([^"]+)"`; the capture equals `$TraceFile`.
- `debug plugin name matches the build`: `Split-Path -Leaf $RemoteDebugSprx` equals `'nativepadps3_vsh_debug.sprx'` and `src/build.ps1` contains (single-quoted, no interpolation) `'"nativepadps3_$Variant$suffix.sprx"'`.

- [ ] **Step 2: Run it to verify it fails**

Run: `pwsh -File tools/test_console_lib.ps1` -> `FAIL` lines, exit 1.

- [ ] **Step 3: Implement the lib additions**

The Produces block; `Get-Trace` uses `$TraceFile`.

- [ ] **Step 4: Run the self-test** -> every case `PASS`, exit 0.

- [ ] **Step 5: Rework `tools/ps3_console_deploy.ps1`**

- Default `-Sprx (Join-Path $PSScriptRoot '../src/nativepadps3_vsh_debug.sprx')`; `$remoteSprx = $RemoteDebugSprx`.
- Backup: download `$ConfigFile` to `prev_config_$stamp.txt`; if that curl fails, download `$LegacyConfigFile` to `prev_legacy_config_$stamp.txt`; if both fail, `Say 'No config on the console (neither folder)'` and continue. `Say` which (`backed up the new-folder config` / `backed up the legacy config`) and its contents.
- Upload size check: `-match "\s$local\s.*$([regex]::Escape((Split-Path -Leaf $remoteSprx)))$"`.
- Config: written locally as `nativepadps3.txt` in `$Dest`, uploaded with `Curl-Ok @('--ftp-create-dirs', '-T', $cfg, "$ftp$ConfigFile")`.
- `boot_plugins.txt` (step 3) **replaces**: `$cur = @(Get-Content $bp)`; if `-not (Test-BootPluginSingle $cur $remoteSprx)`, upload `@(Set-BootPluginEntry $cur $remoteSprx) -join "`n"`; then **always** re-download to `boot_plugins.check.txt` and `throw 'boot_plugins.txt must list exactly one plugin line, the new one'` unless `Test-BootPluginSingle (Get-Content $chk) $remoteSprx`; `Say` the result.
- Trace (step 4): listing match and RNFR/RNTO use `$TraceFile` and `nativepadps3_boot_$stamp.txt`; old-name traces are not touched.
- `.DESCRIPTION`: replaces any old or new plugin line with the new one, creates the config folder, nothing on the console is deleted. Reboot, wait for `loop`, peeks and power-off sites unchanged.

- [ ] **Step 6: Restore, test, trace, linkkeys**

- restore: `$keep = @(Remove-BootPluginEntries (Get-Content $cur))`; post-check throws `'a plugin line (old or new name) is still listed in boot_plugins.txt'` if any re-read line matches `$PluginEntryPattern`. Help: both names removed; plugin files and both config folders stay on the disk.
- test.ps1 (does not dot-source the lib; keep it so): defaults `../src/nativepadps3_vsh_debug.sprx` and `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx`; `$dumpFiles` and the `$done` keys `nativepadps3_lv2.bin`, `nativepadps3_lv2.txt`, `nativepadps3_lv2_b.bin`, `nativepadps3_lv2_b.txt`; `$trace = 'nativepadps3_boot.txt'`; `$out = "nativepadps3_boot_btcheck_$stamp.txt"`; help line 11 `DUMP_LV2 = 1 in nativepadps3.txt`.
- trace.ps1 help line 7: `/dev_hdd0/tmp/nativepadps3_boot.txt`. linkkeys.ps1:62 popup text `'NativePadPS3: power the pads on now'`.

- [ ] **Step 7: Offline checks**

Run: `pwsh -File tools/test_console_lib.ps1` -> all `PASS`, exit 0.
Run, for each `tools/ps3_console_*.ps1` (parse only): `$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path <file>), [ref]$null, [ref]$e); $e.Count` -> `0`.
Run: `git grep -n -i -E "ds45pad|DS45:" -- tools` -> only the two labelled lib lines, the two labelled fixtures in `test_console_lib.ps1`, `shim_pack.py:372` and `lv2scan.py` docstrings (those two are step 2's).
Do **not** run deploy/restore/test/trace/linkkeys against any address.

- [ ] **Step 8: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add tools/`; subject `tools: nativepadps3 names; deploy replaces the boot entry, restore strips both`.

---

### Task 5: Shipped configs, `.gitignore`, README and user docs

**Files:**
- Move (`git mv`): `ps3_ready/plugins/ds45pad/ds45pad.txt` -> `ps3_ready/plugins/nativepadps3/nativepadps3.txt`; `ps3_ready/plugins/ds45pad/ds45pad_devices.txt` -> `ps3_ready/plugins/nativepadps3/nativepadps3_devices.txt`
- Modify: `ps3_ready/boot_plugins.txt`; both moved files' header lines 2-3
- Modify: `.gitignore:11` (add `nativepadps3_boot*.txt` on the next line; keep `ds45pad_boot*.txt`, a legacy name)
- Modify: `README.md` lines 33, 71, 75, 77, 102, 118-124, 157, plus a new `### Upgrading from ds45pad` subsection at the end of "🚀 Installation (passthrough path)"
- Modify: `docs/BUILD.md`, `docs/COMPILAR.md`, `docs/INSTALAR_CFW.md`, `docs/INSTALAR_HEN.md`, `docs/PEDIR_BUILD.md`, `docs/DIAGNOSTICO.md`, `docs/ANALOGICOS.md`, `docs/ARQUITETURA.md`, `docs/TESTE_PS3XPAD.md`
- Do not touch: `README.md:9` and `:175`, `CREDITS.md`, `recovery/boot_plugins.txt`, `docs/superpowers/**`, `patches/ps_only.patch`

- [ ] **Step 1: `ps3_ready/` and `.gitignore`**

Header line 2: `# NativePadPS3 - Configuration File` / `# NativePadPS3 - Recognized USB Controllers`; line 3 the new console path. `ps3_ready/boot_plugins.txt` = exactly `/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx`. `git status` shows two renames.

- [ ] **Step 2: README**

Paths, file names, install steps (step 1 says create `/dev_hdd0/plugins/nativepadps3/`), the `boot_plugins.txt` line and the build output `src/nativepadps3_vsh.sprx`. The upgrade subsection: the old `/dev_hdd0/plugins/ds45pad/` folder keeps working (the plugin falls back to it when the new folder has no `nativepadps3.txt`; the boot trace shows `datadir_legacy`); to migrate, create `/dev_hdd0/plugins/nativepadps3/` and copy `ds45pad.txt` there as `nativepadps3.txt` (optionally `ds45pad_devices.txt` -> `nativepadps3_devices.txt`, `ds45pad_cal.txt` -> `nativepadps3_cal.txt`); from then on the new folder wins; in `boot_plugins.txt` **replace** the old plugin line, never add a second line. Write the subsection so that every line containing an old name contains no other `ds45`-prefixed identifier (Task 8 excludes these lines by exact content).

- [ ] **Step 3: User docs**

Every hit of `git grep -n -i -E "ds45pad|DS45Pad|DS45PAD" -- ':(glob)docs/*.md'` changes per Global Constraints, and product prose `DS45Pad`/`DS45PAD` -> `NativePadPS3`: `BUILD.md` title `# Building NativePadPS3`, paths/outputs, the scetool example `-e nativepadps3.prx nativepadps3_vsh.sprx` (leave `DS45_TOOLS` for Task 10); `COMPILAR.md` (stale `release/plugins/ds45pad/` -> `release/plugins/nativepadps3/`); `INSTALAR_CFW.md`; `INSTALAR_HEN.md` (stale `data/ds45pad.txt` -> `ps3_ready/plugins/nativepadps3/nativepadps3.txt`, stale toast `DS45PAD 1.0.0 carregado` -> `NativePadPS3 Caricato / Loaded`); `PEDIR_BUILD.md`; `DIAGNOSTICO.md` (config name, `/dev_hdd0/tmp/nativepadps3_diag.txt`); `ANALOGICOS.md` (`/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt`); `ARQUITETURA.md` (`nativepadps3_game.sprx`); `TESTE_PS3XPAD.md:18` (delete or comment out the NativePadPS3 line; older installs: the `ds45pad` line). Keep each doc's language (Portuguese).

- [ ] **Step 4: Check**

Run: `git grep -n -i -E "ds45pad|DS45Pad" -- ps3_ready README.md ':(glob)docs/*.md'` -> only `README.md` 9 and 175, the upgrade subsection, the `TESTE_PS3XPAD.md` older-installs clause.

- [ ] **Step 5: Commit**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add -A ps3_ready .gitignore README.md docs/*.md`; subject `docs, ps3_ready: NativePadPS3 names and the ds45pad upgrade path`.

---

### Task 6: Step-1 verification gate (offline)

**Files:** none unless the gate finds a miss (fix it in its owner file; commit `fix: leftover ds45pad reference in <file>`).

- [ ] **Step 1:** `pwsh -File tests/run_tests.ps1` -> no `FAIL`, exit 0. `pwsh -File tools/test_console_lib.ps1` -> all `PASS`. `pwsh -File tools/test_check_no_proprietary.ps1` -> all `PASS`. `python -I -m pytest tools/test_shim_pack.py tools/test_shim_stubs.py tools/tests/` -> all pass.
- [ ] **Step 2:** Task 3 Steps 2 and 3 on this tree -> as there.
- [ ] **Step 3: Step-1 grep gate.** Run (PowerShell tool):
```powershell
git grep -n -E "ds45pad|DS45Pad|DS45:" -- src shim tools tests ps3_ready README.md ':(glob)docs/*.md' |
  Where-Object { ($_ -creplace 'ds45pad\.h', '') -cmatch 'ds45pad|DS45Pad|DS45:' }
git grep -n -E '"[^"]*DS45PAD[^"_]*"' -- src shim tools tests
```
(Case-sensitive on purpose: uppercase `DS45PAD` banners, `DS45PAD_VERSION` and `DS45PAD_H` are step 2's; the second command catches an uppercase old name left inside a string literal and must print nothing.)
Expected residue of the first command, and nothing else: the labelled legacy lines in `src/paths.c`, `tests/test_paths.c`, `tools/ps3_console_lib.ps1`, `tools/test_console_lib.ps1`; `tools/lv2scan.py` docstrings; `README.md` 9, 175 and the upgrade subsection; `docs/TESTE_PS3XPAD.md` older-installs clause; `DS45Pad` prose in code comments (step 2 renames it).
- [ ] **Step 4:** `pwsh -File tools/check_no_proprietary.ps1` -> exit 0; `git status --short` -> clean.

---

### Task 7: Console stage A - functional rename (controller runs it with the user; NOT delegated)

The spec requires a console stage after **each** step ("After step 1" and "After step 2"), so this plan has two: this one, before step 2 starts, and Task 12, last. Stage A is where the legacy-fallback check happens; stage B proves the post-rename build still loads. The controller dispatches no sub-agent for either and never contacts the console: the **user** runs every command and pastes the output; the controller checks it against the expectations. Hardware: DEX 4.92. Data under `tools/dumps/dex492/rename1/` (git-ignored).

**Console safety (binding for Tasks 7 and 12):**
- No peek bursts: only the tools' throttled peeks; never a hand-written peek loop.
- Arm DualSenses by BD address only (`ps3_console_linkkeys.ps1 -Arm -Bd <address>`), never the Move motion controller's address; addresses are never written into the repo.
- Every console session ends with `tools/ps3_console_restore.ps1`, even after a failure.
- If the console hangs: hold the physical power button. Never hard reset.

- [ ] **A1 Before state.** User confirms `boot_plugins.txt` lists `/dev_hdd0/plugins/ds45pad_vsh_debug.sprx`, the old folder holds the live config, `/dev_hdd0/plugins/nativepadps3/` does not exist (FTP listing of `/dev_hdd0/plugins/`).
- [ ] **A2 Deploy.** Build: `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (controller). User: `pwsh tools/ps3_console_deploy.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`. Expected: `backed up the legacy config`; config uploaded (folder created); exactly one plugin line, `/dev_hdd0/plugins/nativepadps3_vsh_debug.sprx`; reboot; `Plugin in its loop`.
- [ ] **A3 Trace, new folder.** User: `Select-String -Path tools/dumps/dex492/rename1/boot.txt -Pattern '^start|^1\.0\.1|datadir_|config_bytes|config_NOT_FOUND|kshim|^loop'`. Expected in order: `start`, version, `datadir_new`, `config_bytes=...`, `kshim` lines as before the rename, `loop`; no `config_NOT_FOUND_using_defaults`. Pads OFF: `pwsh tools/ps3_console_linkkeys.ps1 -Ip <console> -Arm -Bd <DualSense address(es)>`; pads on; the DualSense behaves exactly as before (DS3 route, rumble, power-off); `pwsh tools/ps3_console_trace.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1` shows `shimdbg` lines.
- [ ] **A4 Legacy fallback.** User: `curl.exe -Q "RNFR /dev_hdd0/plugins/nativepadps3/nativepadps3.txt" -Q "RNTO /dev_hdd0/plugins/nativepadps3/nativepadps3.txt.off" ftp://<console>/`, the same RNFR/RNTO for `/dev_hdd0/tmp/nativepadps3_boot.txt` -> `nativepadps3_boot_legacy-before.txt` (the trace is appended across boots), then `curl.exe http://<console>/reboot.ps3`. Back up: `pwsh tools/ps3_console_trace.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1` and `Select-String -Path tools/dumps/dex492/rename1/boot_now.txt -Pattern 'datadir_|config_bytes|config_NOT_FOUND'`. Expected: `datadir_legacy`, `config_bytes` of the old `ds45pad.txt`, no `config_NOT_FOUND`; arm by BD address again; the pad behaves the same. Then rename `nativepadps3.txt.off` back, stamp the trace again, reboot: the trace shows `datadir_new`.
- [ ] **A5 Restore.** User: `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename1`. Expected: `after:` has no `ds45pad` or `nativepadps3` line; reboot; event slot stock; `CLEAN: no shim installed, the hooks are stock.`
- [ ] **A6 Record.** Controller writes `docs/superpowers/notes/2026-11-08-nativepadps3-rename-console.md` (section "Stage A": commands, markers seen, pad behaviour, deviations; no BD addresses, no dumps). `pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git commit -F`. Step 2 starts only after stage A passes.

---

## Step 2 - mechanical prefix rename `ds45` -> `npp`

### Task 8: Rename script, its self-test, and the blob baseline

**Files (all git-ignored scratch, never committed):**
- Create: `.superpowers/rename/npp_rename.ps1`, `.superpowers/rename/test_npp_rename.ps1`, `.superpowers/rename/compare_blob.ps1`
- Create: `.superpowers/rename/baseline/` (pre-rename `shim_blob.h`, `shim.elf`, section dumps, `baseline.json`)

**Interfaces:**
- Produces: `pwsh -File .superpowers/rename/npp_rename.ps1 [-Root <repo>] [-DryRun]` — exit 0 on success, 1 on any check failure; prints one report line per file (`<path>: <n> changes [rule counts]`), a per-rule total, the list of excluded lines found, the masked-token counts, and the planned `git mv`s. Without `-DryRun` it performs the `git mv`s and writes the files (UTF-8, preserving each file's line endings and trailing newline; no BOM added).
- Produces: `pwsh -File .superpowers/rename/compare_blob.ps1 -Baseline .superpowers/rename/baseline -Header src/shim_blob.h -Elf shim/shim.elf` — exit 0 when identical, 1 otherwise, printing each compared item.

- [ ] **Step 1: Capture the blob baseline (pre-rename tree, i.e. HEAD after Task 7)**

Run (PowerShell tool) `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (regenerates `src/shim_blob.h` and `shim/shim.elf`; the shim is built the same for every variant). Copy both into `baseline/`. Dump `.text`, `.data`, `.toc` of `shim/shim.elf` with `llvm-objcopy -O binary --only-section=<sec> shim/shim.elf baseline/<sec>.bin` (`C:/Program Files/LLVM/bin/llvm-objcopy.exe` if not on PATH; a section that does not exist is recorded as absent). Write `baseline.json` with `BLOB_LEN`, `RELOC_COUNT`, every `*_SHIM_OFF_<NAME>` value keyed by `<NAME>`, SHA-256 of the normalised `blob[]` bytes and of the `relocs[]` values (array body between `= {` and `};`, all numeric tokens parsed and re-serialised as lowercase hex), and SHA-256 of each section dump. `compare_blob.ps1 -Baseline ... ` run against the baseline itself must exit 0.

- [ ] **Step 2: Write the failing self-test `test_npp_rename.ps1`**

It builds a throwaway git repo under `$env:TEMP` (as `tools/test_check_no_proprietary.ps1` does), writes fixture files, runs the script with `-Root` and checks the results. Cases:
- `ordering`: `DS45PAD_VERSION`->`NPP_VERSION`, `DS45PAD_H`->`NPP_H`, `/* DS45PAD - x */`->`/* NPP - x */`, `DS45Pad` and `Ds45Pad`->`NativePadPS3`, `#include "ds45pad.h"`->`#include "npp.h"`, `-DDS45_DEBUG`->`-DNPP_DEBUG`, `ds45_shim_data_t`->`npp_shim_data_t`, `"\taddis 12, 2, ds45_shim_data+"`->`"\taddis 12, 2, npp_shim_data+"`, `ds45stop`->`nppstop`.
- `bytes kept`: a line `#define DS45_SHIM_MAGIC   0x4453343553484D31ULL   /* "DS45SHM1" */` becomes `#define NPP_SHIM_MAGIC   0x4453343553484D31ULL   /* "DS45SHM1" */`; `"DS45K1 0x1"`, `"DS45K0 0x1"`, `"ds45k1 0x1"` unchanged.
- `excluded line`: a fixture file listed with an exact-line exclusion keeps that line byte for byte, other lines in it are renamed; with the exclusion's text drifted by one character the script exits 1 naming the entry.
- `file moves`: `src/ds45pad.h` and `src/ds45_state.h` end up as `src/npp.h` and `src/npp_state.h` (tracked as renames: `git status --porcelain` shows `R`).
- `allowlist`: files under `docs/superpowers/`, `patches/ps_only.patch`, `CREDITS.md`, `LICENSE` are byte-identical afterwards.
- `residue`: a fixture with `DS45x` (uppercase bare, matches no rule) makes the script exit 1 and print the residue line.
- `collision`: `DS45_X` and `DS45PAD_X` in the fixtures (both map to `NPP_X`) make it exit 1; so does a target token already present (`NPP_Y` existing while `DS45_Y` is renamed).
- `idempotent`: a second run reports 0 changes and exits 0.
- `dry run`: `-DryRun` reports the same counts as the real run and leaves the tree unchanged.

- [ ] **Step 3: Run it to verify it fails** -> `pwsh -File .superpowers/rename/test_npp_rename.ps1` reports `FAIL` (script missing), exit 1.

- [ ] **Step 4: Implement `npp_rename.ps1`**

File list: `git -C $Root ls-files`, minus the allowlist (`CREDITS.md`, `LICENSE`, `patches/ps_only.patch`, `docs/superpowers/**`), minus files git treats as binary (keep only paths listed by `git -C $Root grep -I -l -e ''`). Per file, per line:
1. If `(file, exact line)` is in the exclusion table, keep the line (each entry must match exactly once in the tree, else exit 1).
2. Mask the byte-bearing tokens (case-sensitive): `DS45SHM1`, `DS45K1`, `DS45K0`, `ds45k1`.
3. Apply, case-sensitive, plain substring (no word boundaries: `-DDS45_DEBUG` must be hit), in this order:

| # | Find | Replace |
|---|---|---|
| 1 | `DS45PAD_` | `NPP_` |
| 2 | `DS45PAD` | `NPP` |
| 3 | `DS45Pad` | `NativePadPS3` |
| 4 | `Ds45Pad` | `NativePadPS3` |
| 5 | `ds45pad_` | `npp_` |
| 6 | `ds45pad` | `npp` |
| 7 | `DS45_` | `NPP_` |
| 8 | `ds45_` | `npp_` |
| 9 | `ds45` | `npp` |

4. Unmask. 5. Residue: any `(?i)ds45` left on a non-excluded line outside the masked tokens -> report and exit 1.
Collision check before writing (case-sensitive, on whole identifier tokens `[A-Za-z0-9_]+`): two distinct source tokens with one target, or a target that already exists in the file list -> exit 1. `git mv src/ds45pad.h src/npp.h` and `git mv src/ds45_state.h src/npp_state.h` before writing their content.

Exclusion table (the implementer fills in the exact current line text from the tree, one entry per line, and the commit message of Task 9 reproduces it): the labelled legacy literal lines in `src/paths.c` (`LEGACY_DIR` definition and the four legacy table entries) and `tests/test_paths.c` (`OLD_CFG`, `OLD_DEV`, `OLD_CAL`, `OLD_KC`); `tools/ps3_console_lib.ps1` `$LegacyConfigFile` and `$PluginEntryPattern` lines; `tools/test_console_lib.ps1` `$old` and `$oldRel` lines; `.gitignore` `ds45pad_boot*.txt`; `README.md` lines 9 and 175 and every line of the "Upgrading from ds45pad" subsection that names an old file; the `docs/TESTE_PS3XPAD.md` older-installs line. Each excluded line must contain no `ds45` token other than a legacy on-console name or a credit (the script checks: after removing `ds45pad` path/file-name substrings and the credit words `DS45Pad`, no `(?i)ds45` remains; otherwise exit 1, because the line needs splitting in its owner file).

- [ ] **Step 5: Run the self-test** -> all `PASS`, exit 0.

- [ ] **Step 6: Dry run on the real tree**

Run: `pwsh -File .superpowers/rename/npp_rename.ps1 -DryRun` -> exit 0. Check the report against the pre-rename occurrence counts (`git grep -o -i ds45 -- . ':!docs/superpowers' ':!patches/ps_only.patch' ':!CREDITS.md' ':!LICENSE' | Group-Object { ($_ -split ':', 2)[0] }`): for every file, replaced occurrences + masked tokens + occurrences on excluded lines = its count. Save the report as `.superpowers/rename/dryrun.txt`. Nothing to commit in this task.

---

### Task 9: Apply the mechanical rename (one commit) and pass the step-2 gates 1-4

**Files:** every tracked text file the script reports; `src/ds45pad.h` -> `src/npp.h`, `src/ds45_state.h` -> `src/npp_state.h`. No hand edits in this task. If a gate fails because of the source tree (an excluded line that needs splitting, a byte-bearing literal not masked), discard the uncommitted script output with `git reset --hard HEAD` (at that point the working tree holds nothing but script output), fix the owner file or the script's tables in a separate commit / in `.superpowers/rename/`, and rerun from Step 1.

**Interfaces:**
- Consumes: Task 8 script, baseline, `compare_blob.ps1`.
- Produces (post-rename names later tasks use): `src/npp.h` (guard `NPP_H`), `src/npp_state.h`, `NPP_DEBUG`/`NPP_GAME`/`NPP_HOST_TEST`, `NPP_VERSION`, `NPP_BOOT_FILE`, `npp_paths_init()`, `npp_path(npp_path_kind_t)`, `NPP_PATH_*`, `NPP_DATADIR_NEW`, `npp_shim_data` / `npp_shim_data_t`, generated `NPP_SHIM_BLOB_H`, `NPP_SHIM_BLOB_LEN`, `NPP_SHIM_RELOC_COUNT`, `NPP_SHIM_OFF_*`, `npp_shim_blob[]`, `npp_shim_relocs[]`, `NPP_SHIM_MAGIC` (value unchanged), `NPP_TOOLS` (env var read by the build scripts).

- [ ] **Step 1: Run the rename** on a clean tree: `git status --short` empty, then `pwsh -File .superpowers/rename/npp_rename.ps1` -> exit 0, same counts as `dryrun.txt`. Then run it again -> 0 changes.

- [ ] **Step 2: Gate 2, host suite.** `pwsh -File tests/run_tests.ps1` -> every test builds (with `-DNPP_HOST_TEST` and the renamed `/* CFLAGS: -DNPP_DEBUG */` lines) and passes, `test_paths` included.

- [ ] **Step 3: Gate 3, Python and guard tests.** `python -I -m pytest tools/test_shim_pack.py tools/test_shim_stubs.py tools/tests/` -> all pass; `pwsh -File tools/test_check_no_proprietary.ps1` -> all `PASS`; `pwsh -File tools/test_console_lib.ps1` -> all `PASS` (its trace case now parses `NPP_BOOT_FILE` out of `src/npp.h`, renamed by the script).

- [ ] **Step 4: Gate 4, four SDK builds.** Task 3 Step 2 commands, with `NPP_TOOLS` unset and `DS45_TOOLS` unset (fself) -> four `Successfully built and signed` lines. Task 3 Step 3 string check on `nativepadps3_vsh_debug.sprx` and `nativepadps3_game_debug.sprx` -> second list empty (only legacy folder names and `DS45SHM1`/`DS45K1` carry `ds45`).

- [ ] **Step 5: Gate 1, blob byte identity.** The VSH build in Step 4 regenerated `src/shim_blob.h` and `shim/shim.elf`. Run `pwsh -File .superpowers/rename/compare_blob.ps1 -Baseline .superpowers/rename/baseline -Header src/shim_blob.h -Elf shim/shim.elf` -> exit 0: `BLOB_LEN` equal, `RELOC_COUNT` equal, every `OFF_<NAME>` value equal by name suffix, `blob[]` and `relocs[]` hashes equal, `.text`/`.data`/`.toc` hashes equal. Second method, must agree: diff the baseline header and the new header after `-creplace 'DS45_|NPP_',''`, `-creplace 'ds45_|npp_',''` and dropping the banner line -> no difference. Any mismatch stops the task.

- [ ] **Step 6: Commit (mechanical only)**

`pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add -A` (the tree contains only the script's changes and the two renames); subject `rename: code prefix ds45 -> npp (mechanical, scripted)`. Body: the replacement table, the masked tokens, the full exclusion table (file + exact line), the per-rule totals, and "shim blob byte-identical (BLOB_LEN <n>, OFF_* equal, blob/reloc/section hashes equal)". If the script found another byte-bearing literal, it is listed here and kept.

---

### Task 10: Lockstep hardening (hand-written)

**Files:**
- Modify: `tools/ps3_console_lib.ps1:57-69` (power-off offsets), `tools/test_console_lib.ps1`
- Modify: `src/build.ps1:5,14,18` (`NPP_TOOLS` guard), `src/build.sh:19-21` (same), `docs/BUILD.md` (only if the script left a sentence that no longer reads right)

**Interfaces:**
- Consumes: Task 9 names (`NPP_SHIM_OFF_POWEROFF`, `NPP_SHIM_OFF_TEARDOWN`, `NPP_TOOLS`).
- Produces: `function Get-PoOffsets([string]$HeaderPath)  # -> @{ Poweroff = <int>; Teardown = <int> }`; throws `"$HeaderPath has no NPP_SHIM_OFF_POWEROFF/TEARDOWN defines (stale or foreign shim_blob.h; rebuild with src/build.ps1)"` when the file exists but either define is missing. The lib calls it when `src/shim_blob.h` exists; only when the file is absent does it keep the DEX 4.92 defaults (`0x130`/`0x1D0`), and then it says so with `Write-Host` on dot-source.

- [ ] **Step 1: Failing self-test cases** in `tools/test_console_lib.ps1`:
- `po offsets parsed`: temp header with `#define NPP_SHIM_OFF_POWEROFF 0x140 /* x */` and `#define NPP_SHIM_OFF_TEARDOWN 0x1e0 /* y */` -> `@{Poweroff=0x140; Teardown=0x1E0}`.
- `po offsets loud on old prefix`: temp header with the same lines but `DS45_SHIM_OFF_*` -> throws.
- `po offsets loud on missing teardown`: only the POWEROFF line -> throws.
- `po offsets match the build` (only when `src/shim_blob.h` exists): `$PoOffs` after dot-sourcing equals `Get-PoOffsets src/shim_blob.h`.
- `build.ps1 legacy DS45_TOOLS fails loudly`: in a child `pwsh` with `DS45_TOOLS='C:\nowhere'` and `NPP_TOOLS` removed, `pwsh -NoProfile -File src/build.ps1 -Sdk C:\nowhere` exits non-zero and its output contains `NPP_TOOLS`; with both unset it fails on the SDK check instead (message does not contain `NPP_TOOLS`). No SDK, no build.

Run `pwsh -File tools/test_console_lib.ps1` -> those cases `FAIL`.

- [ ] **Step 2: Implement.** `Get-PoOffsets` per Produces and use it in the lib. `src/build.ps1`: `[string]$Tools = $env:NPP_TOOLS`; the very first statement after `$ErrorActionPreference = 'Stop'` is the guard: if `$env:DS45_TOOLS` is set and `$env:NPP_TOOLS` is not -> `throw 'DS45_TOOLS was renamed NPP_TOOLS: set NPP_TOOLS to the same folder (and unset DS45_TOOLS). No SPRX was built.'`; comments and the scetool error mention `NPP_TOOLS`. `src/build.sh`: `TOOLS="${NPP_TOOLS:-}"` and, before anything else, `if [ -n "${DS45_TOOLS:-}" ] && [ -z "${NPP_TOOLS:-}" ]; then echo "ERRO: DS45_TOOLS was renamed NPP_TOOLS: set NPP_TOOLS to the same folder. No SPRX was built." >&2; exit 1; fi`. The guard lines get a comment `legacy variable name, one-cycle migration check; remove after the next release`.

- [ ] **Step 3: Verify.** `pwsh -File tools/test_console_lib.ps1` -> all `PASS`. Bash tool, from the repo: `DS45_TOOLS=/nowhere bash src/build.sh; echo "exit=$?"` (run with `cd src` first) -> the ERRO line and `exit=1`. One real build: `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` -> `Successfully built and signed`.

- [ ] **Step 4: Commit.** `pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git add tools/ps3_console_lib.ps1 tools/test_console_lib.ps1 src/build.ps1 src/build.sh docs/BUILD.md`; subject `tools, build: loud shim offsets, NPP_TOOLS with a loud DS45_TOOLS check`.

---

### Task 11: Final offline gates (whole branch)

**Files:** none unless a gate fails (fix in the owner file, commit `fix: ...`, rerun every gate).

- [ ] **Step 1:** Gates 2 and 3 again (Task 9 Steps 2-3) plus the Task 10 self-test -> all pass.
- [ ] **Step 2:** Gate 4 again (four builds + string check) and Gate 1 again (`compare_blob.ps1` against the baseline) -> exit 0.
- [ ] **Step 3: Gate 5, grep gate.** Run (PowerShell tool) `git grep -n -i ds45` over all tracked files and drop `docs/superpowers/` and `patches/ps_only.patch` hits:
```powershell
git grep -n -i ds45 -- . ':!docs/superpowers' ':!patches/ps_only.patch' | Format-Table -AutoSize -Wrap
```
Expected, and nothing else: `CREDITS.md`; `README.md` credit lines 9 and 175 and the upgrade subsection; `docs/TESTE_PS3XPAD.md` older-installs line; `.gitignore` `ds45pad_boot*.txt`; the labelled legacy literals in `src/paths.c`, `tests/test_paths.c`, `tools/ps3_console_lib.ps1`, `tools/test_console_lib.ps1`; `shim/shim_abi.h` `NPP_SHIM_MAGIC` value comment `"DS45SHM1"`; `tests/test_ds3report.c` `"DS45SHM1"` (comment and literal); `src/kernel.c` `CACHE_MAGIC "DS45K1"`, `src/kernel.h` cache-format comment `"DS45K1 ..."`; `tests/test_kernel.c` `DS45K1`/`DS45K0`/`ds45k1` literals; the `DS45_TOOLS` migration-check lines in `src/build.ps1` and `src/build.sh` and the two `test_console_lib.ps1` cases that set it. Anything else is a missed rename. Then `git ls-files | Select-String -Pattern '(?i)ds45' | Where-Object { $_ -notmatch '^docs/superpowers/' }` -> nothing.
- [ ] **Step 4: Gate 6, tools dry check.** With `src/shim_blob.h` present: `pwsh -NoProfile -Command '$Ip = "offline.invalid"; . ./tools/ps3_console_lib.ps1; "{0:X} {1:X}" -f $PoOffs.Poweroff, $PoOffs.Teardown'` -> the two `NPP_SHIM_OFF_*` values from `src/shim_blob.h`, no fallback message.
- [ ] **Step 5:** `pwsh -File tools/check_no_proprietary.ps1` -> exit 0; `git status --short` -> clean.

---

### Task 12: Console stage B - the post-rename build must still load (controller + user; NOT delegated; last)

Same rules and safety block as Task 7. Data under `tools/dumps/dex492/rename2/`. Only the final build (after Task 11) is deployed.

- [ ] **B1 Deploy.** Controller builds `pwsh -File src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell`. User: `pwsh tools/ps3_console_deploy.ps1 -Ip <console> -Dest tools/dumps/dex492/rename2`. Expected: `backed up the new-folder config`; exactly one plugin line (the new one); reboot; `Plugin in its loop`; the power-off site read-back uses the parsed offsets (entries printed as `0x57C000 + NPP_SHIM_OFF_POWEROFF/TEARDOWN` values from `src/shim_blob.h`), first site `hook`.
- [ ] **B2 Trace and pad.** Same `Select-String` as A3 on `rename2/boot.txt`: `start`, version, `datadir_new`, `config_bytes`, `kshim` accepted with the same magic/version as stage A (no refusal line), `loop`; arm by BD address, pads on, the DualSense behaves as in stage A; `ps3_console_trace.ps1` shows `shimdbg`. A missed rename would show as no trace, no `loop`, or a kshim refusal: then restore (B3) before anything else.
- [ ] **B3 Restore.** `pwsh tools/ps3_console_restore.ps1 -Ip <console> -Dest tools/dumps/dex492/rename2` -> `CLEAN`. The console ends stock.
- [ ] **B4 Record.** Controller adds section "Stage B" to `docs/superpowers/notes/2026-11-08-nativepadps3-rename-console.md` and updates the spec's `Status:` line (both stages passed, or what failed). `pwsh -File tools/check_no_proprietary.ps1` (exit 0); `git commit -F`. Then the controller shows the user the full "Rulings I made" list and moves to finishing.
