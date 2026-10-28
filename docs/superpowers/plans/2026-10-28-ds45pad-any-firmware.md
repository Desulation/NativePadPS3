# DS45Pad Any-Firmware Bluetooth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the hardcoded CEX 4.93 Bluetooth kernel address with a verified-kernel table, a cache, a load-time LV2 scan and a passive cross-check, in a new repository that contains no Sony-proprietary material.

**Architecture:** All LV2 reads go through one bounds-checked choke point (`ds45_lv2_read`). `kernel.c` resolves a `ds45_kernel_t` once at startup (table → cache → scan → legacy → none). `btcheck.c` decides from passive VSH observations whether the raw report can be trusted; `passthru.c` registers the virtual pad only after it passes. A debug-only dumper and `tools/lv2scan.py` produce new table entries.

**Tech Stack:** C (Cell SDK `ppu-lv2-gcc`, `-mprx`), host unit tests with LLVM clang 15 (native Windows executables), PowerShell 7, Python 3.12 + pytest (+ optional capstone for disassembly display).

**Spec:** `docs/superpowers/specs/2026-10-28-ds45pad-any-firmware-design.md`

## Global Constraints

- New repository at `C:\Users\gman2\source\AiTesting\ImprovedDualSensePs3\DualSense-PS3-Improved`, fresh history; the upstream clone `DualSense-PS3` is never modified again after Task 1. Nothing is pushed to any remote without the user's go-ahead.
- Never committed: SDK files, scetool keys/`data/`, PUPs, decrypted kernels, LV2 dumps, `kernel_cache.txt`, ProDG files, `*.sprx *.prx *.sym *.self *.elf *.a *.bin *.pup`. Fingerprints are FNV-1a 64 hashes; scan patterns are instruction masks.
- Credits: `CREDITS.md` names domyno6773/DualSense-PS3 (commit `33ba29e`, v2.0.0) as the original project, imported via Desulation/DualSense-PS3; carries Dobridp, OsirisX, aldostools, PS3Xploit Team; domyno6773's donation link only under their name. GPLv2 `LICENSE` and all existing headers unchanged.
- LV2 window: `DS45_LV2_BASE = 0x8000000000000000`, `DS45_LV2_BOUND = 0x8000000000800000` (8 MiB, the range webMAN MOD's `/dump.ps3?lv2` reads). No peek outside `[BASE, BOUND - 8]`, none unaligned.
- No-match toast text, exact: `DS45Pad: Bluetooth not supported on this kernel`. Dumper toast, exact: `LV2 dump written`.
- Cross-check constants: 50 matching changes to pass; sticks within ±2; report must freeze within 200 ms of disconnect.
- Debug build: `build.ps1 -Debug` / `make DEBUG=1` define `DS45_DEBUG` and output `ds45pad_<variant>_debug.sprx`. Release builds contain no dumper and no debug trace strings.
- The game build (`DS45_GAME`) gains no new source files and no new imports.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Bluetooth pad connects before the plugin finishes the scan** (scan runs before the poll loop, ~1-3 s). Expected: the pad is adopted on the first rescan after resolution, not lost. → test `test_passthru_scan_waits_for_resolution` (Task 7).
2. **`bt_work` slot still NULL at first poll** (Bluetooth stack not up yet). Expected: retried every rescan tick, never cached as failure. → test `test_null_slot_retried` (Task 7).
3. **Cache file written by an older plugin or truncated by power loss.** Expected: ignored, scan runs, cache rewritten. → test `test_cache_malformed_lines` (Task 4).
4. **Same physical controller unplugged from USB while cross-check is pending.** Expected: cross-check restarts from zero on the next bind; a disconnect during PENDING is not a FAIL. → test `test_disconnect_while_pending_resets` (Task 6).
5. **Two scan hits that resolve to the same slot and offset** (pattern occurring twice in one function, e.g. inlined). Expected: treated as one candidate, not ambiguity. → test `test_scan_duplicate_hits_same_result` (Task 5).

---

### Task 1: New repository with proprietary-material guard

**Files:**
- Create (by export): whole tree of `DualSense-PS3` at `33ba29e` into `DualSense-PS3-Improved/`
- Delete in new repo: `src/*.sprx`, `ps3_ready/plugins/ds45pad/*.sprx`, `.github/FUNDING.yml`
- Create: `CREDITS.md`, `tools/check_no_proprietary.ps1`, `tools/test_check_no_proprietary.ps1`, `.githooks/pre-commit`
- Modify: `.gitignore`, `README.md`
- Move: `docs/superpowers/specs/2026-10-28-ds45pad-any-firmware-design.md` and this plan from the clone into the new repo

**Interfaces:**
- Produces: `pwsh tools/check_no_proprietary.ps1 [-Staged] [-Root <path>]` → exit 0 clean, exit 1 with one `BLOCKED: <path>: <reason>` line per offence.

- [ ] **Step 1: Export the tree without git history**

```bash
mkdir ../DualSense-PS3-Improved
git -C ../DualSense-PS3 archive --format=tar 33ba29e | tar -x -C ../DualSense-PS3-Improved
```
Then delete the files listed above and copy the untracked spec and plan from the clone.

- [ ] **Step 2: Write the failing guard test `tools/test_check_no_proprietary.ps1`**

Creates a temp git repo and asserts, by exit code and output:
- clean repo with one `.c` file → exit 0
- staged `x.sprx` → exit 1, output contains `BLOCKED: x.sprx: banned extension`
- staged `tools/dumps/a.txt` → exit 1, `banned path`
- staged `kernel_cache.txt` → exit 1, `banned path`
- staged text file containing the SCE-prefixed confidential marker (`'SCE ' + 'CONFIDENTIAL'`) → exit 1, `SDK marker`
- staged binary file `x.dat` (contains a NUL byte) → exit 1, `binary not allowlisted`

Run: `pwsh tools/test_check_no_proprietary.ps1` → Expected: FAIL (script missing).

- [ ] **Step 3: Implement `tools/check_no_proprietary.ps1`**

Rules: banned extensions `.sprx .prx .sym .self .elf .a .bin .pup .pkg .rap .edat .o`; banned paths `tools/dumps/`, any segment named `keys`, `kernel_cache.txt`, `sdk_doc_chm/`; SDK markers (case-sensitive) `'SCE ' + 'CONFIDENTIAL'`, `'Sony Computer Entertainment Inc. ' + 'All Rights Reserved'`, `'Sony Interactive Entertainment Inc. ' + 'All Rights Reserved'` (written split in source so the files themselves do not contain them); binary = NUL byte in the first 8000 bytes, allowlist empty. `-Staged` checks `git diff --cached --name-only --diff-filter=ACMR`; otherwise `git ls-files -co --exclude-standard`.

- [ ] **Step 4: Run guard test** → Expected: `PASS` for all six cases.

- [ ] **Step 5: Hook, ignore rules, credits, README**

- `.githooks/pre-commit`: `#!/bin/sh` + `exec pwsh -NoProfile -File tools/check_no_proprietary.ps1 -Staged`; activate with `git config core.hooksPath .githooks`.
- `.gitignore` adds: `*.sprx *.prx *.self *.elf *.a *.bin *.pup tools/dumps/ kernel_cache.txt __pycache__/ .pytest_cache/ tests/bin/`.
- `CREDITS.md` per Global Constraints, including both reasons for the independent continuation (scope; SDK-linked, retail-signed `.sprx` in upstream history carry Sony IP risk).
- `README.md`: replace the top caution block's donation/support section with a link to `CREDITS.md`; Installation says to build from source (`ps3_ready/` keeps only `.txt` files); add the "Why there are no binaries in this repository" section (SDK stub code, leaked retail keys, build output only, pointer to `tools/check_no_proprietary.ps1`).

- [ ] **Step 6: Whole-tree check** — Run: `pwsh tools/check_no_proprietary.ps1` → Expected: exit 0.

- [ ] **Step 7: Initial commit**

```bash
git init && git config core.hooksPath .githooks && git add -A
git commit -m "Import DualSense-PS3 v2.0.0 source (domyno6773/DualSense-PS3@33ba29e) without binaries

Independent continuation; see CREDITS.md.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Native host test harness

**Files:**
- Delete: `tests/run_bt_native.ps1`, `tests/test_bt_native.c`, `tests/stubs/string.h`
- Create: `tests/run_tests.ps1`, `tests/fakes.c`, `tests/fakes.h`, `tests/test_smoke.c`
- Modify: `tests/stubs/cell/pad.h`, `tests/stubs/sys/process.h`

**Interfaces:**
- Produces: `pwsh tests/run_tests.ps1 [-Filter <name>]` compiles each `tests/test_*.c` with `clang -std=c99 -Wall -Wextra -Werror -DDS45_HOST_TEST -I tests/stubs -I src` together with `tests/fakes.c` and the `src/*.c` files listed in a `/* SOURCES: a.c b.c */` comment on the test's first line, into `tests/bin/<name>.exe`, runs it, and prints `PASS <name>` / `FAIL <name>:<line>`; exit 1 on any failure.
- Produces in `tests/fakes.h`: `uint64_t fake_now_us;` `void fake_lv2_set(uint64_t addr, const uint8_t *bytes, int len);` `void fake_lv2_reset(void);` `int fake_lv2_out_of_bounds;` (count of reads outside the window), `int fake_toasts; char fake_last_toast[128];` (via a fake `ds45_notify`), no-op fakes for `ds45_boot_mark` / `ds45_boot_mark_hex`, file fakes backing `ds45_read_file` / `ds45_write_file` / `ds45_delete_file` / `ds45_file_exists` (`fake_file_set(path, text)`, `fake_file_get(path)`, `fake_file_deleted(path)`), LDD fakes `int fake_ldd_registers;`, and `void fake_reset_all(void);`.
- Produces in stubs: `CellPadData { int32_t len; uint16_t button[64]; }`, `CELL_PAD_MAX_CODES 64`, `CELL_PAD_OK 0`, `CellPadInfo2` (existing), `system_call_N` / `return_to_user_prog` macros routed to `fake_syscall(n, a1..a4)`.
- Test macro in `fakes.h`: `#define CHECK(e) do { if (!(e)) return __LINE__; } while (0)`; each test file defines `int run_tests(void)`; `fakes.c` provides `main` that prints the result.

- [ ] **Step 1: Write `tests/test_smoke.c`** asserting `fake_lv2_set` round-trips one qword and `fake_toasts == 0` after `fake_reset_all()`.
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1` → Expected: FAIL (harness missing).
- [ ] **Step 3: Implement harness, fakes, stubs.** `fake_syscall(6, addr)` serves `ds45_peek`: returns the big-endian qword at `addr` from the fake image, counts `fake_lv2_out_of_bounds` when `addr` is outside `[BASE, BOUND-8]`.
- [ ] **Step 4: Run** → Expected: `PASS test_smoke`.
- [ ] **Step 5: Commit** `test: native clang host test harness replacing stale wasm test`.

---

### Task 3: Debug build switch and LV2 read choke point

**Files:**
- Modify: `src/build.ps1`, `src/Makefile`, `src/ds45pad.h`, `src/util.c`
- Create: `src/lv2.h`, `src/lv2.c`
- Test: `tests/test_lv2.c`

**Interfaces:**
- Produces in `src/lv2.h`:
  - `#define DS45_LV2_BASE 0x8000000000000000ULL`, `#define DS45_LV2_BOUND 0x8000000000800000ULL`
  - `uint64_t ds45_peek(uint64_t addr);` (syscall 6; the only syscall-6 call site in the tree)
  - `int ds45_lv2_read(uint64_t addr, uint64_t *out);` → 0 ok; -1 if `addr & 7` or outside `[BASE, BOUND-8]` (no peek issued)
  - `int ds45_lv2_read_bytes(uint64_t addr, uint8_t *out, int len);` → big-endian bytes, `len` multiple of 8, -1 on any out-of-window qword (no partial peeks)
  - `uint64_t ds45_fnv1a64(const uint8_t *p, int n);` (offset basis `0xCBF29CE484222325`, prime `0x100000001B3`)
- Produces in `src/ds45pad.h`: `DS45_TRACE(tag)` / `DS45_TRACE_HEX(tag, v)` → `ds45_boot_mark*` under `DS45_DEBUG`, `((void)0)` otherwise; `DS45PAD_VERSION` gets `"-debug"` appended under `DS45_DEBUG`.
- Produces in `src/util.c`: `int ds45_write_file(const char *path, const void *data, int len);` (create/truncate), `int ds45_delete_file(const char *path);`, `int ds45_file_exists(const char *path);`.

- [ ] **Step 1: Failing tests** in `tests/test_lv2.c` (`/* SOURCES: lv2.c */`):
  - `ds45_lv2_read(DS45_LV2_BASE - 8, &v) == -1` and `ds45_lv2_read(DS45_LV2_BOUND - 4, &v) == -1` and `ds45_lv2_read(DS45_LV2_BASE + 3, &v) == -1`, with `fake_lv2_out_of_bounds == 0` afterwards (no peek issued)
  - `ds45_lv2_read(DS45_LV2_BOUND - 8, &v) == 0`
  - `ds45_fnv1a64("", 0) == 0xCBF29CE484222325ULL`, `ds45_fnv1a64("a", 1) == 0xAF63DC4C8601EC8CULL`
  - `ds45_lv2_read_bytes` returns bytes in memory order (byte 0 = MSB of the first qword)
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter lv2` → FAIL.
- [ ] **Step 3: Implement `lv2.c`, `ds45pad.h` macros, file helpers.** `build.ps1` gains `[switch]$Debug`: adds `-DDS45_DEBUG`, output name suffix `_debug`; `lv2` is added to the VSH source list only. `Makefile`: `DEBUG=1` mirrors it.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat: bounded LV2 read choke point and debug build switch`.

---

### Task 4: Kernel table matching and cache

**Files:**
- Create: `src/kernel.h`, `src/kernel.c`
- Test: `tests/test_kernel.c`

**Interfaces:**
- Consumes: `ds45_lv2_read*`, `ds45_fnv1a64`, `ds45_read_file`, `ds45_write_file`, `ds45_delete_file` (Task 3).
- Produces in `src/kernel.h`:
```c
typedef struct {
    const char *name;
    uint64_t    fp_addr;     /* 0 = no fingerprint (legacy entry) */
    uint64_t    fp_hash;     /* FNV-1a 64 of the 32 bytes at fp_addr */
    uint64_t    slot_addr;
    uint32_t    report_off;
} ds45_kernel_t;
typedef enum { DS45_KMATCH_NONE = 0, DS45_KMATCH_TABLE, DS45_KMATCH_CACHE,
               DS45_KMATCH_SCAN, DS45_KMATCH_LEGACY } ds45_kmatch_t;
#define DS45_FP_LEN 32
#define DS45_KERNEL_CACHE_FILE DS45_DIR "/kernel_cache.txt"
int  kernel_fingerprint_matches(uint64_t fp_addr, uint64_t fp_hash);
const ds45_kernel_t *kernel_match_table(const ds45_kernel_t *tab, int n);
int  kernel_cache_load(ds45_kernel_t *out);   /* 0 only if the line parses AND the fingerprint re-matches */
int  kernel_cache_store(const ds45_kernel_t *k);
void kernel_cache_delete(void);
extern const ds45_kernel_t ds45_kernel_table[];
extern const int           ds45_kernel_table_count;
```
- Cache line format, exact: `DS45K1 <fp_addr> <fp_hash> <slot_addr> <report_off>\n`, all values `0x`-prefixed lowercase hex; name in the cache record is the fixed string `"cached"`.
- Table contents in this task: one legacy entry `{ "CEX 4.93 (legacy)", 0, 0, 0x800000000034C530ULL, 0xE8 }` (= `0x34FBB0 - 0x3680`). `kernel_match_table` skips entries with `fp_addr == 0`.

- Fingerprinted table entries are wrapped in `#ifndef DS45_NO_TABLE` (a test-only define used in Task 12); the legacy entry is always present.

- [ ] **Step 1: Failing tests** (`/* SOURCES: lv2.c kernel.c */`; file helpers come from `fakes.c`):
  - `test_table_hit`: image with known 32 bytes at `0x8000000000123400`, table entry with their FNV hash → returns that entry
  - `test_table_miss_wrong_hash` → NULL
  - `test_table_fp_out_of_window` (fp_addr ≥ BOUND) → NULL and `fake_lv2_out_of_bounds == 0`
  - `test_cache_roundtrip`: store, then load returns identical fields
  - `test_cache_stale_fingerprint`: stored record whose fingerprint no longer matches → load returns -1
  - `test_cache_malformed_lines` (Review Focus 3): `""`, `"DS45K0 ..."`, a line cut mid-field, extra fields, non-hex → all return -1
  - `test_cache_delete` → `fake_file_deleted(DS45_KERNEL_CACHE_FILE)`
- [ ] **Step 2: Run** `-Filter kernel` → FAIL.
- [ ] **Step 3: Implement** `kernel.c` table/cache functions.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat: fingerprinted kernel table and validated cache`.

---

### Task 5: Load-time LV2 scan

**Files:**
- Modify: `src/kernel.h`, `src/kernel.c`
- Test: `tests/test_scan.c`

**Interfaces:**
- Produces:
```c
typedef struct { uint32_t mask, value; } ds45_insn_match_t;
typedef struct {
    const ds45_insn_match_t *insns;   /* consecutive instruction words */
    int count;
    int toc_ld_index;      /* insn `ld rT, DS(r2)`: displacement = (int16_t)(word & 0xFFFC) */
    int report_off_index;  /* insn whose (int16_t)(word & 0xFFFF) is the report offset */
} ds45_scan_pattern_t;
#define DS45_INSN_LD_FROM_R2 { 0xFC1F0003u, 0xE8020000u }  /* ld any, ds(r2) */
#define DS45_INSN_STDU_R1    { 0xFFFF0003u, 0xF8210001u }  /* stdu r1, ds(r1) */
int kernel_scan(const ds45_scan_pattern_t *pat, ds45_kernel_t *out); /* 0 = exactly one distinct result */
extern const ds45_scan_pattern_t ds45_scan_pattern;   /* count == 0 until Task 11 */
```
- Algorithm (not determined by the signature):
  1. Walk `[BASE, BOUND)` in 8-byte reads, testing both 32-bit words of each qword against the pattern (window carried across reads; never re-read).
  2. For each hit: function start = nearest preceding `DS45_INSN_STDU_R1` within 0x800 bytes; none → discard hit.
  3. TOC = second qword of the first 8-aligned qword pair `{start, t}` in the window with `t` inside `[BASE, BOUND)` (the LV2 OPD `{entry, toc, env}`); none → discard hit.
  4. Result = `{ fp_addr = address of insns[toc_ld_index], fp_hash = FNV of 32 bytes there, slot_addr = TOC + disp, report_off }`.
  5. Distinct results compared on `(slot_addr, report_off)`; exactly one → success; zero or more than one → -1. `pat->count == 0` → -1 with no peeks.

- [ ] **Step 1: Failing tests** on a synthetic image (hand-encoded words; no real kernel bytes):
  - `test_scan_single_hit`: prologue `stdu`, a 3-word pattern including `ld r9,-0x3680(r2)` (`0xE922C980`) and `addi r3,r9,0xE8` (`0x386900E8`), an OPD pointing at the prologue with TOC `0x800000000034FBB0` → `slot_addr == 0x800000000034C530`, `report_off == 0xE8`
  - `test_scan_no_hit` → -1
  - `test_scan_ambiguous`: two hits with different displacements → -1
  - `test_scan_duplicate_hits_same_result` (Review Focus 5): two hits resolving to the same slot/offset → 0
  - `test_scan_no_prologue` and `test_scan_no_opd` → -1
  - `test_scan_never_leaves_window`: after every test, `fake_lv2_out_of_bounds == 0`
  - `test_scan_empty_pattern` → -1 and zero peeks
- [ ] **Step 2: Run** `-Filter scan` → FAIL.
- [ ] **Step 3: Implement** `kernel_scan`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat: bounded load-time LV2 signature scan`.

---

### Task 6: Passive cross-check

**Files:**
- Create: `src/btcheck.h`, `src/btcheck.c`
- Test: `tests/test_btcheck.c`

**Interfaces:**
- Produces:
```c
typedef struct { uint8_t lx, ly, rx, ry, l2, r2, hat; uint16_t d1, d2; int ps; } ds45_btraw_t;
int bt_report_decode(uint64_t w0, uint64_t w1, ds45_btraw_t *out); /* -1 unless bytes 0-1 are A1 01 and hat <= 8 */
typedef enum { BTCHECK_PENDING = 0, BTCHECK_PASSED, BTCHECK_FAILED } btcheck_state_t;
#define BTCHECK_MATCHES_REQUIRED 50
#define BTCHECK_MISMATCH_LIMIT   10
#define BTCHECK_STICK_TOLERANCE  2
#define BTCHECK_FREEZE_US        200000
#define BTCHECK_RAW_HISTORY      8
void btcheck_reset(int src_port);
void btcheck_observe_vsh(uint32_t port, const CellPadData *d);          /* from VSH detours */
btcheck_state_t btcheck_feed_raw(const ds45_btraw_t *r, int port_connected, uint64_t now_us);
btcheck_state_t btcheck_state(void);
int  btcheck_vsh_samples_seen(void);
```
- Rules: `bt_report_decode` keeps today's byte layout from `ds45_inject_bt_pad` (lx = w0>>40 … b2 = w1>>56) and maps buttons to `DS45_CTRL_*` the same way. A VSH observation counts only when it differs from the previous one for that port. Each counted observation is compared with the last `BTCHECK_RAW_HISTORY` raw samples: match = any sample with all four sticks within ±2 and `d1 & ~DS45_CTRL_PS`, `d2` equal. 50 matches → PASSED. More than 10 mismatches before passing → FAILED. Raw changes more than 200 ms after `port_connected` went 0 → FAILED. A disconnect while PENDING resets counters (not a failure). The VSH sample counter behind `btcheck_vsh_samples_seen()` and the source port survive a FAILED state; only `btcheck_reset` clears them (Task 11's gate depends on this, because on a DEX kernel the legacy entry fails first).

- [ ] **Step 1: Failing tests:**
  - `test_decode_rejects_report_ids`: `0xA131…`, `0xA111…` → -1; `0xA101808080800800`, w1=0 → 0, sticks 0x80, hat 8
  - `test_decode_rejects_bad_hat` (hat 9) → -1
  - `test_pass_after_50_matches` (49 → PENDING, 50 → PASSED)
  - `test_tolerance_2` (stick off by 2 matches; by 3 mismatches)
  - `test_ps_bit_ignored` (raw has PS, VSH doesn't → still a match)
  - `test_fail_after_11_mismatches`
  - `test_unchanged_vsh_samples_not_counted`
  - `test_freeze_after_disconnect` (change at +250 ms → FAILED; at +150 ms → still PENDING)
  - `test_disconnect_while_pending_resets` (Review Focus 4)
  - `test_vsh_counter_survives_failure` (after FAILED, `btcheck_vsh_samples_seen()` still increments on new VSH samples)
- [ ] **Step 2: Run** `-Filter btcheck` → FAIL.
- [ ] **Step 3: Implement** `btcheck.c`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat: passive raw-report cross-check against VSH pad reads`.

---

### Task 7: Resolution chain and integration

**Files:**
- Modify: `src/kernel.c`, `src/kernel.h`, `src/passthru.c`, `src/main.c`
- Test: `tests/test_resolve.c`, `tests/test_passthru.c`

**Interfaces:**
- Consumes: Tasks 3-6.
- Produces:
```c
ds45_kmatch_t kernel_resolve(ds45_kernel_t *out);   /* table -> cache -> scan (stores cache) -> legacy -> NONE */
const ds45_kernel_t *kernel_active(void);           /* NULL until resolved or when NONE */
ds45_kmatch_t kernel_active_source(void);
void kernel_reject_active(void);                    /* cross-check failed: delete cache, active = NULL */
void kernel_log_fw(void);  /* DS45_TRACE of PS3MAPI 0x7777/0x0013 FW_TYPE and 0x0014 FW_VERSION; logging only */
```
- `passthru.c`: the literal `0x800000000034FBB0` and `peekq` are removed; reads use `kernel_active()->slot_addr` / `report_off` via `ds45_lv2_read`, with `w0` re-read after `w1` and the sample dropped if it changed (torn read). The `bt_work` pointer must lie in `[BASE, BOUND)`; a NULL or out-of-window slot value is retried on the next rescan tick, never cached. `passthru_scan` binds a slot but calls `ldd_register` only once `btcheck_state() == BTCHECK_PASSED`; on FAILED it calls `kernel_reject_active()`, `passthru_release()` and stops adopting pads. With `kernel_active() == NULL`, `passthru_scan` returns 0.
- `main.c`: `kernel_log_fw(); kernel_resolve(...)` runs after `usb_init` and before the loop; the announce toast shows `DS45Pad: Bluetooth not supported on this kernel` when the source is NONE or a cross-check has failed, else the existing loaded text. `vsh_getdata_hook` / `vsh_getdataextra_hook` call `btcheck_observe_vsh(port_no, data)` **before** `silence_pad_data`. Each rescan tick, the main loop traces `DS45_TRACE_HEX("btcheck_vsh_samples", btcheck_vsh_samples_seen())`, plus `kmatch=<n>` once after resolution and `btcheck=<state>` on every state change.

- [ ] **Step 1: Failing tests:**
  - `test_resolve.c`: order table > cache > scan > legacy; a scan success writes the cache; NONE when all fail; `kernel_reject_active` deletes the cache file
  - `test_passthru.c` (`/* SOURCES: lv2.c kernel.c btcheck.c passthru.c */`, LDD and cellPad fakes):
    - `test_no_virtual_pad_before_pass`: bound pad, 49 matches → `fake_ldd_registers == 0`; 50th → 1
    - `test_none_registers_nothing`: `kernel_active() == NULL` → `passthru_scan() == 0`
    - `test_fail_steps_aside`: forced FAILED → cache deleted, pad released, `passthru_scan() == 0` afterwards
    - `test_torn_read_dropped`: fake changes w0 between reads → sample not fed
    - `test_null_slot_retried` (Review Focus 2): slot reads 0 at first, valid later → raw reads begin, no cache write of a failure
    - `test_passthru_scan_waits_for_resolution` (Review Focus 1): pad connected before resolution → adopted on the first scan after it
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1` → new tests FAIL.
- [ ] **Step 3: Implement** the integration.
- [ ] **Step 4: Run** → all PASS.
- [ ] **Step 5: Commit** `feat: resolve Bluetooth kernel offsets at load and gate on cross-check`.

---

### Task 8: Debug LV2 dumper

**Files:**
- Create: `src/lv2dump.c`
- Modify: `src/ds45pad.h` (`int dump_lv2;` in `ds45_config_t`), `src/config.c` (`DUMP_LV2` key), `src/main.c`, `src/build.ps1`, `src/Makefile` (lv2dump in VSH debug builds only)
- Test: `tests/test_lv2dump.c`

**Interfaces:**
- Produces: `void lv2dump_run(void);` (declared only under `DS45_DEBUG`). Files: `/dev_hdd0/tmp/ds45pad_lv2.bin` (exactly `BOUND - BASE` bytes, written in 64 KiB chunks from `ds45_lv2_read_bytes`) and `/dev_hdd0/tmp/ds45pad_lv2.txt` with lines `base=`, `bound=`, `version=`, `fw_type=`, `fw_version=`, `kmatch=`. Toast `LV2 dump written`. Returns immediately if the `.bin` exists. `main.c` calls it once at `ANNOUNCE_MS` when `g_cfg.dump_lv2`, under `#ifdef DS45_DEBUG` only.

- [ ] **Step 1: Failing tests:** `test_dump_writes_both_files` (size = 8 MiB, first/last qword correct), `test_dump_skips_when_exists` (no writes, no toast), `test_dump_reads_stay_in_window` (`fake_lv2_out_of_bounds == 0`).
- [ ] **Step 2: Run** `-Filter lv2dump` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat: debug-build LV2 dumper`.

---

### Task 9: `tools/lv2scan.py`

**Files:**
- Create: `tools/lv2scan.py`, `tools/requirements.txt` (`pytest`, `capstone`), `tools/tests/test_lv2scan.py`, `tools/tests/synth.py`

**Interfaces:**
- Produces (Python):
  - `fnv1a64(data: bytes) -> int` (same constants as C)
  - `find_toc(img: bytes, base: int, func_start: int) -> int | None` (same OPD rule as Task 5)
  - `find_candidates(img: bytes, base: int) -> list[Candidate]`; `Candidate` is a dataclass `(fp_addr, fp_hash, slot_addr, report_off, func_start, disasm: list[str])`. Rule: a function (prologue-delimited) containing compare-immediates `0x054C`, `0x0268` and `0xA1`; for each `ld rT, ds(r2)` in it, `report_off` = immediate of the first later D-form instruction with `rA == rT` and immediate in `[0x10, 0x400]`.
  - `common_pattern(per_dump: list[tuple[bytes, Candidate]]) -> list[tuple[int, int]]`: aligned instruction window (8 words before to 8 after `fp_addr`), per word mask = bits equal across all dumps, then `toc_ld_index`/`report_off_index` reported.
  - `emit_c_entry(name: str, c: Candidate) -> str` and `emit_c_pattern(...) -> str` producing paste-ready C.
  - CLI: `python tools/lv2scan.py DUMP [DUMP ...] [--name NAME ...] [--base 0x8000000000000000]`; capstone used only to fill `disasm` when importable.

- [ ] **Step 1: Install tooling** `python -m pip install -r tools/requirements.txt`.
- [ ] **Step 2: Failing tests** with `synth.py` building images from hand-encoded words: `test_fnv_matches_c_vectors` (same two vectors as Task 3), `test_candidate_found`, `test_no_candidate_without_vid_pid`, `test_common_pattern_masks_out_displacement` (two images differing only in the `ld` displacement → that word's low 16 bits masked), `test_emit_c_entry_format`, `test_runs_on_local_dumps` (skipped unless `tools/dumps/*.bin` exist).
- [ ] **Step 3: Run** `python -m pytest tools/tests -q` → FAIL.
- [ ] **Step 4: Implement** `lv2scan.py`.
- [ ] **Step 5: Run** → PASS (local-dump test skipped).
- [ ] **Step 6: Commit** `feat: lv2scan.py offline candidate and pattern finder`.

---

### Task 10: Build verification

**Files:** none new (fixes only, if a build fails).

- [ ] **Step 1:** Ask the user for `DS45_TOOLS` (scetool directory with `data/keys`); never copy it into the repo.
- [ ] **Step 2:** Run `pwsh src/build.ps1 -Variant vsh`, `-Variant vsh -Debug`, `-Variant game`, `-Variant game -Debug` → each prints `Successfully built and signed`.
- [ ] **Step 3:** `strings` check: release `ds45pad_vsh.sprx` contains none of `LV2 dump written`, `ds45pad_lv2`, `kmatch=`; debug build contains them. The game build's import list (`ppu-lv2-prx-info`) is unchanged versus a build of the Task 1 tree.
- [ ] **Step 4:** `pwsh tools/check_no_proprietary.ps1` → exit 0 (built `.sprx` files are ignored, not tracked).
- [ ] **Step 5: Commit** any build fixes: `build: verify release and debug variants`.

---

### Task 11: On-console data collection and table entries  *(user + agent)*

**Files:**
- Modify: `src/kernel.c` (table entries, `ds45_scan_pattern`), possibly `src/lv2.h` (`DS45_LV2_BOUND` only if evidence requires)

- [ ] **Step 1 (user, DEX 4.92):** Install the debug VSH build with `DUMP_LV2 = 1`, pair the DualSense over Bluetooth, use the XMB for 30 s, then copy `/dev_hdd0/tmp/ds45pad_boot.txt`, `ds45pad_lv2.bin`, `ds45pad_lv2.txt` into `tools/dumps/dex492/`.
- [ ] **Step 2 (gate):** In the boot trace, `btcheck_vsh_samples_seen()` (traced as `btcheck_vsh_samples`) must be > 0. **If it is 0, stop and bring it back to the user**: the passive cross-check cannot work and the spec's weaker fallback needs their decision.
- [ ] **Step 3 (user, DECR DEX 4.75):** Repeat step 1 into `tools/dumps/dex475/`.
- [ ] **Step 4:** Run `python tools/lv2scan.py tools/dumps/dex492/ds45pad_lv2.bin tools/dumps/dex475/ds45pad_lv2.bin --name "DEX 4.92" --name "DEX 4.75"`; review candidates with the user; paste the two entries and the common pattern into `kernel.c`.
- [ ] **Step 5:** Run `pwsh tests/run_tests.ps1` and `python -m pytest tools/tests -q` → PASS (local-dump test now runs).
- [ ] **Step 6: Commit** `data: DEX 4.92 and DEX 4.75 kernel entries and scan pattern` (pre-commit hook confirms no dump is staged).

---

### Task 12: On-console verification  *(user + agent)*

- [ ] **Step 1 (both consoles, debug build):** Boot trace shows `kmatch=1` (table) and cross-check `PASSED`; PS button opens the XMB menu over Bluetooth.
- [ ] **Step 2:** Build a debug variant with the table entries compiled out (`-DDS45_NO_TABLE`, test-only define): first boot traces `kmatch=3` (scan) and writes `kernel_cache.txt` with the same slot/offset as the table; second boot traces `kmatch=2` (cache).
- [ ] **Step 3:** Build with a deliberately wrong pattern word and no table/cache: console does not panic, no Bluetooth virtual pad, USB pad works, toast `DS45Pad: Bluetooth not supported on this kernel`.
- [ ] **Step 4:** Release build on both consoles: same behaviour as Step 1, no `ds45pad_lv2*` files created.
- [ ] **Step 5:** Record results in `docs/superpowers/specs/2026-10-28-ds45pad-any-firmware-design.md` §5 as a dated "Verified on" note and commit `docs: record on-console verification`.
