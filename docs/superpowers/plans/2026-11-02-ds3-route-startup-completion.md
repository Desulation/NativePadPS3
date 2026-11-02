# DS3-route Start-up Completion and Basic Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DualSense registered as a Sony pad completes the DS3 driver's Bluetooth start-up, keeps its pad object, gets a native port, and delivers buttons, sticks and analog triggers through the DS3 path.

**Architecture:** The shim blob's start-up hook is replaced by a wrapper on the start-up driver's event handler (`shim_event`), which, for a pad object identified as `054C/0CE6`, writes a DS3 info template, sets status 0 and substate 9 and returns 0. The input hook identifies the pad from the embedded transport object and translates the DualSense's basic `0x01` report into the DS3 report. Product-ID acceptance becomes pure kernel word edits (helper always; inline only behind a debug config key). All connection tracking and the feature-0x05 send are removed.

**Tech Stack:** C99 (freestanding PPC64 blob built with clang 15 `--target=powerpc64-unknown-linux-gnu` + ld.lld; plugin with the Cell SDK `ppu-lv2-gcc`), host tests with native clang (`pwsh tests/run_tests.ps1`), Python 3.12 packer (`tools/shim_pack.py`), PowerShell console scripts (webMAN peek/poke/FTP).

**Spec:** `docs/superpowers/specs/2026-11-01-ds45pad-ds3-route-startup-completion-design.md` (amends `docs/superpowers/specs/2026-10-30-ds45pad-native-ds3-route-design.md`). Evidence: SDD ledger `.superpowers/sdd/2026-10-30-ds45pad-native-ds3-route/progress.md`, sections "READ-ONLY (b) DONE", "CONSOLE CAPTURE 2026-11-01", "START-UP DRIVER MAPPED".

## Global Constraints

- Works like a DS3 or better; never asks the user anything; verification is passive only.
- All kernel writes are RAM-only, on fingerprinted kernels only (DEX 4.92 first), verified against the expected original before writing and read back after. The shim stays behind the `ENABLE_SHIM` gate (default 0).
- Nothing Sony-proprietary in git: no SDK files, keys, dumps, kernel code bytes or SDK-linked binaries. Expected original kernel code is stored as hashes; kernel addresses and small device constants (version numbers, flags, calibration values) may be stored as values.
- A DS3, and any other device, is untouched.
- Staged: basic input first, with no requests sent by our kernel code.
- Shim code rules (spec 1): never blocks or allocates, no unbounded loops, touches only DualSense connections, bounds-checked against the actual read length, needs nothing from the plugin process after install.
- Builds: all four variants (`vsh`/`game` x release/debug) build; the shim is VSH-only (absent from game builds). Host tests: `pwsh tests/run_tests.ps1` all PASS before every commit.
- Console work: webMAN at most 2-3 requests per second (a ~30 Hz peek loop hung the console on 2026-11-01); fast sampling uses the plugin's `CAPTURE_PTR`/`CAPTURE_LV2` capture.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `feat/any-firmware`; never push without asking.

## Review Focus

1. A DS3 connecting over Bluetooth while the shim is installed must start up through the original handler, every event, with nothing written to its driver or pad (Task 3 test `test_event_ds3_untouched`).
2. A DualSense that disconnects and reconnects gets a new driver object and must complete again; an event after completion (substate already 9) must go to the original (Task 3 tests `test_event_dualsense_completes`, `test_event_after_done_passes`).
3. A driver whose pad pointer is null, or points at memory that is not a live Sony pad (wrong vtable), must reach the original with no writes (Task 3 test `test_event_bad_pad_untouched`).
4. A DualSense read that is short, empty or not report `0x01` must pass through unchanged with no out-of-bounds read (Task 2 fuzz `test_basic_fuzz_all_lengths`, Task 3 `test_input_passthrough`).
5. Plugin reload while the shim is installed must adopt the existing blob through the event slot, and unload must restore the event and input slots and the helper word (Task 3 tests `test_adopt_via_event_slot`, `test_remove_restores_all`).

---

### Task 1: Settle the start-up completion facts (read-only research)

**Files:**
- Create: `docs/superpowers/notes/2026-11-02-ds3-route-startup-completion-facts.md`

**Interfaces:**
- Consumes: the DEX 4.92 dumps in `tools/dumps/dex492/` (git-ignored; never committed), `analysis/ff_492.json`, the workspace tools `fn.py`/`dis.py` (run with `uv run --no-project --with capstone` from a directory that does not contain the workspace `dis.py`, which shadows the stdlib module).
- Produces (each a section in the note, with its evidence and decision):
  - **C1 info template:** the DS3 info block bytes pad+0x80..+0xD5 (0x56 bytes) from the DS3 Bluetooth pad object in `ds3route/navpid_connected.bin` (object `0x661000`), and, for each field, the kernel code that reads it (at least `0x2A3AD8` check, `0x2A08A4` port assignment, the converter `0x2A4678`). Decision: fields that are read get the DS3 values; fields that must be per-pad (e.g. address bytes +0x08..+0x0F, if read) are named; if any exist, stop and report before Task 3 - the plugin supplying per-pad bytes is a design addition the user must approve.
  - **C2 transport embedding:** whether pad+0x4F0 holds the transport object (16 bytes, vtable `0x37CE18`, handle at +8) for both pad objects in navpid. Decision: if yes, input identity is `pad = xport - 0x4F0`; if not, say so and stop - the plan's Task 3 input identity must be redesigned.
  - **C3 output on port assignment:** the path that sends the DS3 output report (player LEDs) after a port is assigned, the sender it uses, and what a refused transfer does (retry, ignore, or remove the pad). Decision: if a refusal can remove the pad, Task 4 (conditional) is required.
  - **C4 row values (already measured, re-verify):** event slot `0x800000000037CFF8` holds `0x800000000033A2B0`; event handler entry qword `@0x800000000000CACF0` FNV-1a 64 `0x16E42A0B1EBFEFFA`; helper qword `@0x29E2C8` original `4E8000206880042F`, FNV `0x8D1047F832C76096`, patched instruction at `0x29E2CC` = `0x68800CE6` (qword FNV after patch `0x8CF4E0F832AFE9D9`); input slot `0x800000000037CE38` holds `0x800000000033A010`. Verify against a clean (unpatched) dump such as `ds3route/patched_ds1_on.bin`'s non-patched sites or the any-firmware `lv2_code_492.bin`.

- [ ] **Step 1: Disassemble and read C1-C3** with `fn.py` over `navpid_connected.bin` (functions `0x2A3AD8`, `0x2A08A4`, `0x2A4678`, and the senders reachable from `0x2A08A4`); dump the two pad objects' +0x80..+0xD5 and +0x4F0..+0x500.
- [ ] **Step 2: Verify C4** by hashing the qwords in a clean dump with `ds45_fnv1a64` semantics (FNV-1a 64, offset basis `0xcbf29ce484222325`, prime `0x100000001b3`, bytes in memory order).
- [ ] **Step 3: Write the note** with C1-C4, each with evidence (addresses, instructions) and its decision. The template is written as a C byte array literal ready for Task 3. No dump bytes other than the 0x56-byte DS3 info block values.
- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/notes/2026-11-02-ds3-route-startup-completion-facts.md
git commit -m "docs: start-up completion facts (info template, embedding, output path)"
```

### Task 2: Basic-report translator

**Files:**
- Modify: `shim/ds3_report.h`, `shim/ds3_report.c`
- Test: `tests/test_ds3report.c`

**Interfaces:**
- Consumes: `ds45_shim_cfg_t` (`shim/shim_abi.h`), the existing DS3 output layout in `shim/ds3_report.h`.
- Produces (`shim/ds3_report.h`):

```c
#define DS_BASIC_REPORT_ID  0x01
#define DS_BASIC_LEN        10     /* id + LX LY RX RY + 3 button bytes + L2 R2 */
/* DualSense Bluetooth basic report in[0..in_len) -> DS3 report. 0 and all
 * DS3_REPORT_LEN bytes of out written, or -1 and out untouched when a pointer is
 * NULL, in[0] != DS_BASIC_REPORT_ID or in_len < DS_BASIC_LEN. Reads nothing past
 * in[in_len-1]; reads all input before writing (in-place safe, out == in). */
int ds3_report_from_dualsense_basic(const uint8_t *in, int in_len,
                                    const ds45_shim_cfg_t *cfg,
                                    uint8_t out[DS3_REPORT_LEN]);
```

Basic report layout (DS4-compatible basic layout; confirmed on the console in Task 5 step 3, fixed there if it differs): `[1]` LX `[2]` LY `[3]` RX `[4]` RY; `[5]` low nibble hat (0=Up .. 7=Up-Left clockwise, 8 = none), `0x10` Square `0x20` Cross `0x40` Circle `0x80` Triangle; `[6]` `0x01` L1 `0x02` R1 `0x04` L2 `0x08` R2 `0x10` Create `0x20` Options `0x40` L3 `0x80` R3; `[7]` `0x01` PS `0x02` touchpad click; `[8]` L2 analog `[9]` R2 analog.

Mapping: as `ds3_report_from_dualsense` (Create = Select, Options = Start, PS = PS, touchpad click per `cfg->map_touchpad`, stick deadzone `cfg->deadzone`, pressure L2/R2 from the analog bytes and 0/255 for the others), sensors set to the DS3 centre value `0x0200` big-endian at [41..48]. Factor the shared "decoded buttons/sticks -> DS3 bytes" writer out of `ds3_report_from_dualsense` into a static helper used by both; do not duplicate the bit tables.

- [ ] **Step 1: Write the failing tests** in `tests/test_ds3report.c`:
  - `test_basic_neutral`: `{0x01,0x80,0x80,0x80,0x80,0x08,0,0,0,0}` -> returns 0; out[0]==0x01, out[6..9]==0x80, out[2..4]==0, sensors [41..48] == `02 00 02 00 02 00 02 00`.
  - `test_basic_buttons`: Cross (`[5]=0x28`) -> out[3]&0x40 and pressure byte [24]==255; Options (`[6]=0x20`) -> out[2]&0x08; PS (`[7]=0x01`) -> out[4]&0x01; hat 2 (Right) -> out[2]&0x20.
  - `test_basic_triggers`: `[8]=0x40,[9]=0xFF` -> L2 pressure [18]==0x40, R2 pressure [19]==0xFF.
  - `test_basic_refuses`: NULL in, `in[0]=0x31`, `in_len=9` -> -1 and out unchanged (memcmp against a 0xEE-filled copy).
  - `test_basic_in_place`: translate with out == in (buffer of DS3_REPORT_LEN) -> same bytes as an out-of-place run.
  - `test_basic_fuzz_all_lengths`: for every in_len 0..0x41 and 1000 random buffers each (fixed seed), the call never reads past in_len (input placed at the end of a guarded buffer, as the existing fuzz test does) and returns -1 unless in[0]==0x01 and in_len>=10.
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter ds3report` - expected FAIL (build: `ds3_report_from_dualsense_basic` undeclared).
- [ ] **Step 3: Implement** `ds3_report_from_dualsense_basic` and the shared static writer in `shim/ds3_report.c`.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1` - expected all PASS (the existing `0x31` golden tests unchanged).
- [ ] **Step 5: Commit**

```bash
git add shim/ds3_report.h shim/ds3_report.c tests/test_ds3report.c
git commit -m "feat: translate the DualSense basic Bluetooth report to a DS3 report"
```

### Task 3: Shim v3 and installer v3 (event completion, pad identity, word-edit acceptance)

The shim's data block and the installer share one ABI (`src/kshim.c` writes the block by these offsets), so they change in one task: a commit with only one side would break `test_kshim` and the build.

**Files:**
- Modify: `shim/shim_abi.h`, `shim/shim.h`, `shim/shim.c`, `shim/entry.c`, `tools/shim_pack.py`, `tools/test_shim_pack.py`, `src/kshim.h`, `src/kshim.c`, `src/kshim_table.c`, `src/config.c`, `src/ds45pad.h`, `src/main.c` (only if it references removed symbols)
- Test: `tests/test_shim.c`, `tests/test_kshim.c`, `tests/test_config.c`

**Interfaces:**
- Consumes: Task 2's `ds3_report_from_dualsense_basic`; Task 1 C1 template, C2 (embedding at 0x4F0), C4 values.
- Produces:

`shim/shim_abi.h` - data block version 3 (replaces the v2 layout; every v2-only field and the conn/send fields are removed):

```c
#define DS45_SHIM_VERSION 3
#define DS45_SHIM_INFO_LEN 0x56          /* pad+0x80 .. +0xD5 */
typedef struct {
	uint64_t magic; uint32_t version; uint32_t enabled;
	uint64_t orig_event, orig_input;     /* original descriptors: 0x37CFF8, 0x37CE38 */
	uint64_t pad_vtable;                 /* 0x800000000038CDC8 on DEX 4.92 */
	ds45_shim_cfg_t cfg;
	uint32_t stat_completed, stat_translated, stat_passed, stat_reserved;
	uint8_t  info_tmpl[0x58];            /* first DS45_SHIM_INFO_LEN bytes used */
} ds45_shim_data_t;
```

`shim/shim.h` - remove the GET/send/start-up declarations (`SHIM_HIDP_*`, `SHIM_SEND_*`, `SHIM_START_TRIES_MAX`, `SHIM_F1_*`, `shim_send_fn`, `shim_startup_dualsense`); keep `shim_is_dualsense`, `shim_pid_ok` (stage 1b) and add:

```c
/* Kernel object offsets (DEX 4.92, ledger "START-UP DRIVER MAPPED"). */
#define SHIM_DRV_PAD      0x48   /* u64 pad object */
#define SHIM_DRV_STATUS   0x50   /* u32 status passed to the ready callback */
#define SHIM_DRV_SUBSTATE 0x54   /* u8, 9 = done */
#define SHIM_DRV_INFO     0x58   /* u64 -> pad+0x80 */
#define SHIM_DRV_DONE     9
#define SHIM_PAD_ID       0x68   /* big-endian VID then PID */
#define SHIM_PAD_XPORT    0x4F0  /* embedded transport object */
/* 1 if pad is a live Sony pad object (u64 at pad+0 == d->pad_vtable) whose
 * pad+0x68 reads 054C/0CE6, else 0. NULL pad -> 0. */
int shim_pad_is_dualsense(const ds45_shim_data_t *d, const uint8_t *pad);
/* Event-handler decision. For an enabled shim and a DualSense driver whose
 * substate != 9: copy d->info_tmpl[0..DS45_SHIM_INFO_LEN) to the info block,
 * store status 0 (4 bytes) and substate 9, bump stat_completed, return 1 (the
 * caller returns 0 to the framework). Otherwise write nothing and return 0
 * (the caller tail-calls the original). drv NULL -> 0. */
int shim_event_complete(ds45_shim_data_t *d, uint8_t *drv);
```

`shim_input_filter` keeps its signature; for a DualSense it now tries `ds3_report_from_dualsense_basic` when `buf[0]==0x01`, else the existing `0x31` path.

`shim/entry.c`: `int shim_event(void *drv, long status, long len)` returns 0 when `shim_event_complete` returns 1, else tail-calls `orig_event(drv, status, len)`; `shim_input` identifies the pad as `(uint8_t *)xport - SHIM_PAD_XPORT` via `shim_pad_is_dualsense` (vid/pid passed to the filter as `054C/0CE6`). `shim_startup` and `entry_identity` are deleted.

`tools/shim_pack.py`: required symbol `shim_event` replaces `shim_startup`; header define `DS45_SHIM_OFF_EVENT` replaces `DS45_SHIM_OFF_STARTUP` (the `shim_pid_ok` checks and `DS45_SHIM_OFF_PID_OK` stay).

`src/kshim.h` - the v3 row:

```c
#define DS45_SHIM_PID_PATCHES     1   /* helper word, always */
#define DS45_SHIM_INLINE_PATCHES  2   /* inline words, only with SHIM_INLINE_PID = 1 */
typedef struct {
	const char *name;
	uint64_t fp_addr, fp_hash;
	uint64_t syscall_table; uint32_t spare_syscall; uint64_t spare_slot_expect;
	uint64_t alloc_desc; uint64_t alloc_pool;
	uint64_t flush_desc;
	uint64_t event_slot, event_expect;          /* 0x37CFF8 / 0x33A2B0 OPD */
	uint64_t event_code_addr, event_code_hash;  /* 0xCACF0 / 0x16E42A0B1EBFEFFA */
	uint64_t input_slot, input_expect;          /* 0x37CE38 / 0x33A010 OPD */
	uint64_t pad_vtable;                        /* 0x800000000038CDC8 */
	const uint8_t *info_tmpl;                   /* DS45_SHIM_INFO_LEN bytes (Task 1 C1) */
	ds45_kpatch_t pid_patches[DS45_SHIM_PID_PATCHES];
	ds45_kpatch_t inline_patches[DS45_SHIM_INLINE_PATCHES];
} ds45_shim_kernel_t;
```

`kshim_set_connection` is removed. `g_cfg.shim_inline_pid` (config key `SHIM_INLINE_PID`, VSH builds only, default 0, any non-zero number = 1) selects whether the inline rows are applied.

DEX 4.92 row values: event `0x800000000037CFF8`/`0x800000000033A2B0`, event code `0x80000000000CACF0`/`0x16E42A0B1EBFEFFA`, input as today, `pad_vtable` `0x800000000038CDC8`, helper row `{ DS45_KPATCH_WORD, 0x800000000029E2CCULL, 0x8D1047F832C76096ULL, 0x68800CE6ULL }`, inline rows = today's two WORD rows (`0x11B5E4`, `0x11B5EC`). The F3 send fields and the start-up slot are deleted from the row.

- [ ] **Step 1: Write the failing tests** in `tests/test_shim.c` (drop the start-up/GET tests; fake driver = `uint8_t drv[0xB0]`, fake pad = `uint8_t pad[0x520]` with `*(uint64_t*)pad = d.pad_vtable`, VID/PID bytes `05 4C 0C E6` at 0x68, `*(uint8_t**)(drv+0x48) = pad`, `*(uint8_t**)(drv+0x58) = pad+0x80`, `drv[0x50..0x53] = 80 01 00 0C`):
  - `test_event_dualsense_completes`: returns 1; pad[0x80..0xD5] == info_tmpl; drv[0x50..0x53] == 0; drv[0x54] == 9; stat_completed == 1.
  - `test_event_after_done_passes`: drv[0x54]=9 -> returns 0, no byte of drv or pad changed.
  - `test_event_ds3_untouched`: PID `02 68` -> 0, no change.
  - `test_event_bad_pad_untouched`: pad pointer NULL; then vtable mismatch -> 0, no change.
  - `test_event_disabled`: `d.enabled=0` -> 0, no change.
  - `test_input_basic_translated`: DualSense basic report in buf, len 10 -> returns 49, buf[0]==0x01, stat_translated==1.
  - `test_input_passthrough`: DS3 identity, short report (len 5), id 0x02 -> unchanged buf and len, stat_passed counts each.
  - `test_pad_identity`: `shim_pad_is_dualsense` NULL / wrong vtable / DS3 / DualSense -> 0/0/0/1.
- [ ] **Step 2: Write the failing installer tests** in `tests/test_kshim.c` (rework the existing install/rollback tests to the v3 row; drop the `kshim_set_connection` tests):
  - `test_install_writes_v3`: after install, `0x37CFF8` holds base+`DS45_SHIM_OFF_EVENT`, `0x37CE38` holds base+`DS45_SHIM_OFF_INPUT`, the helper word at `0x29E2CC` is `0x68800CE6`, the data block has version 3, `pad_vtable`, `orig_event = 0x800000000033A2B0`, `info_tmpl` == the row's template; `0x37CFE8` was never written (fake LV2 write log).
  - `test_inline_rows_gated`: `shim_inline_pid=0` -> `0x11B5E4`/`0x11B5EC` untouched; `=1` -> both written.
  - `test_event_code_hash_mismatch`: wrong code hash -> `KSHIM_FAILED`, image byte-identical, nothing allocated beyond the spare-syscall restore.
  - `test_rollback_each_step`: a write failure at each step (helper, event, input, data block) leaves the image byte-identical.
  - `test_adopt_via_event_slot`: a second install with the blob present returns `KSHIM_ALREADY` by following `0x37CFF8`.
  - `test_remove_restores_all`: `kshim_remove` restores event, input and helper word (and inline words when they were applied).
  - In `tests/test_config.c`: `test_shim_inline_pid_key`: default 0; `SHIM_INLINE_PID = 1` -> 1; `= abc` -> 0.
- [ ] **Step 3: Run** `pwsh tests/run_tests.ps1 -Filter shim` (matches test_shim and test_kshim) and `-Filter config` - expected FAIL (undeclared `shim_event_complete`, v3 row fields).
- [ ] **Step 4: Implement the shim side:** data block v3, `shim.h`/`shim.c`, `entry.c`; `tools/shim_pack.py` and `tools/test_shim_pack.py` for `shim_event`/`DS45_SHIM_OFF_EVENT`. Byte stores only for status/substate (big-endian, endian-explicit); the info pointer and pad pointer are read as native u64.
- [ ] **Step 5: Implement the installer side:** row type and DEX 4.92 row (template from Task 1 C1 as a `static const uint8_t` array in `kshim_table.c`); event-code hash check before any write; data block fill with `pad_vtable`, `orig_event`, `orig_input`, `info_tmpl`, cfg; patch order helper -> [inline] -> event -> input; journal and rollback as today; adoption via `event_slot`; the `SHIM_INLINE_PID` key; remove `kshim_set_connection` and every caller.
- [ ] **Step 6: Run** `pwsh tests/run_tests.ps1` and `python -I tools/test_shim_pack.py` - expected all PASS; then build all four variants (`$env:CELL_SDK='C:/usr/local/cell'`; `src/build.ps1 -Variant vsh`, `-Variant vsh -Debug`, `-Variant game`, `-Variant game -Debug`) - expected four "Successfully built" lines; game `.sprx` files contain no `kshim`/`SHIM_INLINE_PID` strings.
- [ ] **Step 7: Commit**

```bash
git add shim/ tools/shim_pack.py tools/test_shim_pack.py src/kshim.h src/kshim.c src/kshim_table.c src/config.c src/ds45pad.h src/main.c tests/test_shim.c tests/test_kshim.c tests/test_config.c
git commit -m "feat: complete a DualSense's DS3 start-up from the event handler"
```

### Task 4 (conditional on Task 1 C3): Swallow DS3 output for a DualSense

Only if the Task 1 note concludes that a refused DS3 output report can remove the pad. Otherwise mark this task "not needed (C3)" in the ledger and skip it.

**Files:** `shim/entry.c`, `shim/shim.h`, `shim/shim.c`, `src/kshim.h`, `src/kshim_table.c`, `src/kshim.c`, `tests/test_shim.c`, `tests/test_kshim.c`

**Interfaces:** one more redirect (the output slot C3 names, verified like the event slot) to `shim_output(...)` with the original's prototype from C3; for `shim_pad_is_dualsense` it returns the original's success value without calling it, otherwise it tail-calls the original. Pure decision `int shim_output_swallow(const ds45_shim_data_t *d, const uint8_t *pad)`.

- [ ] **Step 1: Failing tests** `test_output_dualsense_swallowed` (returns 1), `test_output_ds3_passes` (returns 0); installer test that the output slot is written, rolled back and restored.
- [ ] **Step 2: Run** - FAIL. **Step 3: Implement.** **Step 4: Run** all tests and the four builds - PASS.
- [ ] **Step 5: Commit** - "feat: swallow DS3 output reports for a DualSense".

### Task 5: Console test steps (with the user, DEX 4.92)

**Files:**
- Modify: `tools/ps3_console_test.ps1` (add `-Mode startup`: deploy the debug sprx with a given config, reboot through webMAN, wait for the plugin loop, print the `kshim`/`cap_` trace lines; peeks at no more than 2-3 per second)

**Interfaces:**
- Consumes: Tasks 2-3 (and 4 if done); the debug capture keys `CAPTURE_PTR`, `CAPTURE_LV2`, `CAPTURE_LEN` (commit 5515b6f); the pad manager at `0x51B200` (slots `+0x18`, objects `+0x60`; with a USB DS3 in slot 0 the DualSense's pointer is `0x51B268`).
- Produces: ledger entries with each step's result; the recorded basic report bytes (Task 2 layout confirmed or corrected).

- [ ] **Step 1: Install, DualSense off.** Config `ENABLE=1`, `ENABLE_SHIM=1`, `SHIM_INLINE_PID=0`. Expect trace `kshim_installed`; peek `0x37CFF8` = base+`OFF_EVENT`, `0x37CE38` = base+`OFF_INPUT`, `0x29E2C8` = `4E80002068800CE6`, `0x37CFE8` unchanged `800000000033A208`. A DS3 over USB and over Bluetooth still drives the XMB.
- [ ] **Step 2: Completion.** Add `CAPTURE_PTR=0x51B268`, `CAPTURE_LV2=0x60`, `CAPTURE_LEN=128`; flip the DualSense's link-key type to `0x01` with the pad off; power it on. Pass: the capture shows the object staying for more than 5 s with +0x80/+0x82 = the template's versions and +0xD8 cleared; the data block's `stat_completed` = 1. If no object forms, repeat with `SHIM_INLINE_PID=1` and record that the inline site is needed.
- [ ] **Step 3: Input.** Point the capture at the shared buffer (`CAPTURE_LV2=0x4326C8`, `CAPTURE_LEN=72`, no `CAPTURE_PTR`) and record one raw basic report before translation (temporarily `enabled=0` via a data-block poke, then back to 1). Compare with Task 2's layout; if it differs, fix the translator (Task 2 tests first) before continuing. Then check buttons, sticks and L2/R2 in the XMB and in one game.
- [ ] **Step 4: Together and teardown.** DS3 + DualSense: both have ports and independent input; DualSense off/on three times, each reconnect completes (`stat_completed` increments); plugin unload restores `0x37CFF8`, `0x37CE38` and the helper word.
- [ ] **Step 5: Record** each step's result in the ledger; commit the script change - "test: start-up completion console mode".
