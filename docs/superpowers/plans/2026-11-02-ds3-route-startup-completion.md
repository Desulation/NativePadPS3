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
- Commits end with the `Co-Authored-By:` line for the session's Claude model (recent commits: `Claude Opus 4.8 <noreply@anthropic.com>`); branch `feat/any-firmware`; never push without asking.

## Review Focus

1. A DS3 connecting over Bluetooth while the shim is installed must start up and enable through the original handlers, every event, with nothing written to its drivers or pad (Task 3 tests `test_event_ds3_untouched`, `test_enable_passes`).
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

Amended 2026-11-01 after Task 1 (spec section "Amendment after plan task 1", user decisions): per-pad identity bytes from a counter, input pad found by scanning the pad manager (the transport is a pointer, not embedded), and a second completion hook `shim_enable` on the `0xF4` enable driver's event slot `0x37CFB0`. Task 4 (output wrapper) is not needed (C3).

`shim/shim_abi.h` - data block version 3 (replaces the v2 layout; every v2-only field and the conn/send fields are removed):

```c
#define DS45_SHIM_VERSION 3               /* shipped as 4: pid_orig/pid_applied appended in the Task 3 fix round (176 -> 208 bytes) */
#define DS45_SHIM_INFO_LEN 0x56          /* pad+0x80 .. +0xD5 */
typedef struct {
	uint64_t magic; uint32_t version; uint32_t enabled;
	uint64_t orig_event, orig_enable, orig_input; /* original descriptors: 0x37CFF8, 0x37CFB0, 0x37CE38 */
	uint64_t pad_vtable;                 /* 0x800000000038CDC8 on DEX 4.92 */
	uint64_t pad_mgr;                    /* address of the manager pointer: 0x80000000004A1CB8 */
	ds45_shim_cfg_t cfg;
	uint32_t stat_completed;             /* also numbers the identities: first DualSense = 1 */
	uint32_t stat_enabled, stat_translated, stat_passed;
	uint8_t  info_tmpl[0x58];            /* first DS45_SHIM_INFO_LEN bytes used; identity bytes zero */
} ds45_shim_data_t;
```

`shim/shim.h` - remove the GET/send/start-up declarations (`SHIM_HIDP_*`, `SHIM_SEND_*`, `SHIM_START_TRIES_MAX`, `SHIM_F1_*`, `shim_send_fn`, `shim_startup_dualsense`); keep `shim_is_dualsense`, `shim_pid_ok` (stage 1b) and add:

```c
/* Kernel object offsets (DEX 4.92, ledger "START-UP DRIVER MAPPED", facts note C1-C3). */
#define SHIM_DRV_PAD      0x48   /* u64 pad object (both drivers) */
#define SHIM_DRV_STATUS   0x50   /* u32 status passed to the callback (both drivers) */
#define SHIM_DRV_SUBSTATE 0x54   /* u8, start-up driver: 9 = done */
#define SHIM_DRV_INFO     0x58   /* u64 -> pad+0x80, start-up driver */
#define SHIM_DRV_DONE     9
#define SHIM_EN_DONE      0x96   /* u8, enable driver: 1 = done */
#define SHIM_PAD_ID       0x68   /* big-endian VID then PID */
#define SHIM_PAD_XPORT    0x4F0  /* u64 pointer to the transport object */
#define SHIM_MGR_OBJS     0x60   /* pad manager: u64 object pointers */
#define SHIM_MGR_NOBJS    16
#define SHIM_ID_OFF       0x08   /* identity bytes in the info block: +0x08..+0x0F */
/* 1 if pad is a live Sony pad object (u64 at pad+0 == d->pad_vtable) whose
 * pad+0x68 reads 054C/0CE6, else 0. NULL pad -> 0. */
int shim_pad_is_dualsense(const ds45_shim_data_t *d, const uint8_t *pad);
/* Start-up event decision. For an enabled shim and a DualSense driver whose
 * substate != 9: copy d->info_tmpl[0..DS45_SHIM_INFO_LEN) to the info block,
 * bump stat_completed and write the identity bytes info+0x08..+0x0F =
 * stat_completed (u32 big-endian), 0xD5, 0x00, 0x44, 0x53; store status 0
 * (4 bytes) and substate 9; return 1 (the caller returns 0 to the framework).
 * Otherwise write nothing and return 0 (the caller tail-calls the original).
 * drv NULL -> 0. */
int shim_event_complete(ds45_shim_data_t *d, uint8_t *drv);
/* Enable event decision. For an enabled shim and a DualSense whose enable
 * driver has drv[0x96] == 0: store status 0 (4 bytes) and drv[0x96] = 1, bump
 * stat_enabled, return 1. Otherwise write nothing and return 0. drv NULL -> 0. */
int shim_enable_complete(ds45_shim_data_t *d, uint8_t *drv);
/* The pad object whose +0x4F0 holds xport: mgr = *(u64 *)d->pad_mgr; scan the
 * SHIM_MGR_NOBJS u64 pointers at mgr+0x60, skipping nulls. NULL when d->pad_mgr
 * or mgr is 0, or no entry matches. Pure loads, at most 16 iterations. */
const uint8_t *shim_pad_from_xport(const ds45_shim_data_t *d, const void *xport);
```

`shim_input_filter` keeps its signature; for a DualSense it now tries `ds3_report_from_dualsense_basic` when `buf[0]==0x01`, else the existing `0x31` path.

`shim/entry.c`: `int shim_event(void *drv, long status, long len)` returns 0 when `shim_event_complete` returns 1, else tail-calls `orig_event(drv, status, len)`; `int shim_enable(void *drv, long status, long len)` likewise with `shim_enable_complete` and `orig_enable`; `shim_input` identifies the pad with `shim_pad_from_xport` then `shim_pad_is_dualsense` (vid/pid passed to the filter as `054C/0CE6`). `shim_startup` and `entry_identity` are deleted.

`tools/shim_pack.py`: required symbols `shim_event` and `shim_enable` replace `shim_startup`; header defines `DS45_SHIM_OFF_EVENT` and `DS45_SHIM_OFF_ENABLE` replace `DS45_SHIM_OFF_STARTUP` (the `shim_pid_ok` checks and `DS45_SHIM_OFF_PID_OK` stay).

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
	uint64_t enable_slot, enable_expect;        /* 0x37CFB0 / 0x33A310 OPD */
	uint64_t enable_code_addr, enable_code_hash;/* 0xCB234 / 0xB52798EFCFE165A8 (FNV of the aligned qword 0xCB230; corrected in the final review) */
	uint64_t input_slot, input_expect;          /* 0x37CE38 / 0x33A010 OPD */
	uint64_t pad_vtable;                        /* 0x800000000038CDC8 */
	uint64_t pad_mgr;                           /* 0x80000000004A1CB8 */
	const uint8_t *info_tmpl;                   /* DS45_SHIM_INFO_LEN bytes (facts note C1.5) */
	ds45_kpatch_t pid_patches[DS45_SHIM_PID_PATCHES];
	ds45_kpatch_t inline_patches[DS45_SHIM_INLINE_PATCHES];
} ds45_shim_kernel_t;
```

`kshim_set_connection` is removed. `g_cfg.shim_inline_pid` (config key `SHIM_INLINE_PID`, VSH builds only, default 0, any non-zero number = 1) selects whether the inline rows are applied.

DEX 4.92 row values: event `0x800000000037CFF8`/`0x800000000033A2B0`, event code `0x80000000000CACF0`/`0x16E42A0B1EBFEFFA`, enable `0x800000000037CFB0`/`0x800000000033A310`, enable code `0x80000000000CB234`/`0xB52798EFCFE165A8` (the aligned qword `0xCB230`; corrected in the final review from `0xEF8C3F93F63DFDB3`, the hash of the unaligned 8 bytes), input as today, `pad_vtable` `0x800000000038CDC8`, `pad_mgr` `0x80000000004A1CB8`, `info_tmpl` = the `DS3_INFO_TEMPLATE` array of the facts note C1.5 (copied as a `static const uint8_t` array), helper row `{ DS45_KPATCH_WORD, 0x800000000029E2CCULL, 0x8D1047F832C76096ULL, 0x68800CE6ULL }`, inline rows = today's two WORD rows (`0x11B5E4`, `0x11B5EC`). The F3 send fields and the start-up slot are deleted from the row.

- [ ] **Step 1: Write the failing tests** in `tests/test_shim.c` (drop the start-up/GET tests; fake driver = `uint8_t drv[0xB0]`, fake pad = `uint8_t pad[0x520]` with `*(uint64_t*)pad = d.pad_vtable`, VID/PID bytes `05 4C 0C E6` at 0x68, `*(uint8_t**)(drv+0x48) = pad`, `*(uint8_t**)(drv+0x58) = pad+0x80`, `drv[0x50..0x53] = 80 01 00 0C`):
  - `test_event_dualsense_completes`: returns 1; pad[0x80..0x87] and pad[0x90..0xD5] == info_tmpl; pad[0x88..0x8F] == `00 00 00 01 D5 00 44 53`; drv[0x50..0x53] == 0; drv[0x54] == 9; stat_completed == 1.
  - `test_event_identities_differ`: two fresh DualSense drivers/pads complete in turn -> the second pad's [0x88..0x8B] == `00 00 00 02`, stat_completed == 2.
  - `test_event_after_done_passes`: drv[0x54]=9 -> returns 0, no byte of drv or pad changed.
  - `test_event_ds3_untouched`: PID `02 68` -> 0, no change.
  - `test_event_bad_pad_untouched`: pad pointer NULL; then vtable mismatch -> 0, no change.
  - `test_event_disabled`: `d.enabled=0` -> 0, no change.
  - `test_enable_dualsense_completes` (fake enable driver = `uint8_t en[0x98]`, pad at +0x48, status `80 01 00 0C` at +0x50, en[0x96] = 0): returns 1; en[0x50..0x53] == 0; en[0x96] == 1; stat_enabled == 1; pad unchanged.
  - `test_enable_passes`: DS3 pad, NULL pad, wrong vtable, en[0x96] == 1, `d.enabled=0` -> each 0, no byte changed.
  - `test_pad_from_xport`: fake manager (`uint8_t mgr[0x60 + 16*8]`, `d.pad_mgr` = address of a u64 holding `mgr`) with a DS3 pad in entry 0, null entries, and the DualSense pad in entry 5; each pad's +0x4F0 holds its own fake transport address -> the DualSense's transport finds the DualSense pad, the DS3's finds the DS3 pad, an unknown transport -> NULL, `d.pad_mgr` = 0 -> NULL, manager pointer 0 -> NULL.
  - `test_input_basic_translated`: DualSense pad found through the fake manager, basic report in buf, len 10 -> returns 49, buf[0]==0x01, stat_translated==1.
  - `test_input_passthrough`: DS3 pad, transport not in the manager, short report (len 5), id 0x02 -> unchanged buf and len, stat_passed counts each.
  - `test_pad_identity`: `shim_pad_is_dualsense` NULL / wrong vtable / DS3 / DualSense -> 0/0/0/1.
- [ ] **Step 2: Write the failing installer tests** in `tests/test_kshim.c` (rework the existing install/rollback tests to the v3 row; drop the `kshim_set_connection` tests):
  - `test_install_writes_v3`: after install, `0x37CFF8` holds base+`DS45_SHIM_OFF_EVENT`, `0x37CFB0` holds base+`DS45_SHIM_OFF_ENABLE`, `0x37CE38` holds base+`DS45_SHIM_OFF_INPUT`, the helper word at `0x29E2CC` is `0x68800CE6`, the data block has version 3, `pad_vtable`, `pad_mgr = 0x80000000004A1CB8`, `orig_event = 0x800000000033A2B0`, `orig_enable = 0x800000000033A310`, `info_tmpl` == the row's template; `0x37CFE8` was never written (fake LV2 write log).
  - `test_inline_rows_gated`: `shim_inline_pid=0` -> `0x11B5E4`/`0x11B5EC` untouched; `=1` -> both written.
  - `test_event_code_hash_mismatch` and `test_enable_code_hash_mismatch`: wrong code hash -> `KSHIM_FAILED`, image byte-identical, nothing allocated beyond the spare-syscall restore. Likewise `test_enable_slot_mismatch`: `0x37CFB0` not holding `0x800000000033A310` -> `KSHIM_FAILED`, image byte-identical.
  - `test_rollback_each_step`: a write failure at each step (helper, event, enable, input, data block) leaves the image byte-identical.
  - `test_adopt_via_event_slot`: a second install with the blob present returns `KSHIM_ALREADY` by following `0x37CFF8`.
  - `test_remove_restores_all`: `kshim_remove` restores event, enable, input and helper word (and inline words when they were applied).
  - In `tests/test_config.c`: `test_shim_inline_pid_key`: default 0; `SHIM_INLINE_PID = 1` -> 1; `= abc` -> 0.
- [ ] **Step 3: Run** `pwsh tests/run_tests.ps1 -Filter shim` (matches test_shim and test_kshim) and `-Filter config` - expected FAIL (undeclared `shim_event_complete`, v3 row fields).
- [ ] **Step 4: Implement the shim side:** data block v3, `shim.h`/`shim.c`, `entry.c`; `tools/shim_pack.py` and `tools/test_shim_pack.py` for `shim_event`/`shim_enable`/`DS45_SHIM_OFF_EVENT`/`DS45_SHIM_OFF_ENABLE`. Byte stores only for status, substate, done flag and identity bytes (big-endian, endian-explicit); the info pointer, pad pointer, manager pointer and object pointers are read as native u64.
- [ ] **Step 5: Implement the installer side:** row type and DEX 4.92 row (template from the facts note C1.5 as a `static const uint8_t` array in `kshim_table.c`); event- and enable-code hash checks and slot-value checks before any write; data block fill with `pad_vtable`, `pad_mgr`, `orig_event`, `orig_enable`, `orig_input`, `info_tmpl`, cfg; patch order helper -> [inline] -> event -> enable -> input; journal and rollback as today; adoption via `event_slot`; the `SHIM_INLINE_PID` key; remove `kshim_set_connection` and every caller.
- [ ] **Step 6: Run** `pwsh tests/run_tests.ps1` and `python -I tools/test_shim_pack.py` - expected all PASS; then build all four variants (`$env:CELL_SDK='C:/usr/local/cell'`; `src/build.ps1 -Variant vsh`, `-Variant vsh -Debug`, `-Variant game`, `-Variant game -Debug`) - expected four "Successfully built" lines; game `.sprx` files contain no `kshim`/`SHIM_INLINE_PID` strings.
- [ ] **Step 7: Commit**

```bash
git add shim/ tools/shim_pack.py tools/test_shim_pack.py src/kshim.h src/kshim.c src/kshim_table.c src/config.c src/ds45pad.h src/main.c tests/test_shim.c tests/test_kshim.c tests/test_config.c
git commit -m "feat: complete a DualSense's DS3 start-up from the event handler"
```

### Task 4 (conditional on Task 1 C3): Swallow DS3 output for a DualSense

**Not needed (C3, 2026-11-01):** a refused output report cannot remove the pad. The port gate C3 found instead (the `0xF4` enable driver) is handled by `shim_enable` in Task 3. Kept below for the record.

Only if the Task 1 note concludes that a refused DS3 output report can remove the pad. Otherwise mark this task "not needed (C3)" in the ledger and skip it.

**Files:** `shim/entry.c`, `shim/shim.h`, `shim/shim.c`, `src/kshim.h`, `src/kshim_table.c`, `src/kshim.c`, `tests/test_shim.c`, `tests/test_kshim.c`

**Interfaces:** one more redirect (the output slot C3 names, verified like the event slot) to `shim_output(...)` with the original's prototype from C3; for `shim_pad_is_dualsense` it returns the original's success value without calling it, otherwise it tail-calls the original. Pure decision `int shim_output_swallow(const ds45_shim_data_t *d, const uint8_t *pad)`.

- [ ] **Step 1: Failing tests** `test_output_dualsense_swallowed` (returns 1), `test_output_ds3_passes` (returns 0); installer test that the output slot is written, rolled back and restored.
- [ ] **Step 2: Run** - FAIL. **Step 3: Implement.** **Step 4: Run** all tests and the four builds - PASS.
- [ ] **Step 5: Commit** - "feat: swallow DS3 output reports for a DualSense".

### Task 6: Fixed-base executable placement and exec-probe (spec Amendment 2)

Added 2026-11-02 after console step 1 hung: the blob was allocated in the heap pool `0x22`, which is not executable (lv2 text is `0x0..0x300000`; the heap is above it), so the first kernel call into it faulted. The Evilnat/Cobra payload runs from a fixed executable region with a free zero tail at `0x57BCC8..0x580000`; place the blob there at a fixed base instead of allocating. Runs **before** Task 5. DEX 4.92 (Evilnat/Cobra) only; the shim is fingerprint-gated and the DECR is out of scope (spec "Out of scope").

**Files:**
- Modify: `src/kshim.h`, `src/kshim.c`, `src/kshim_table.c`, `src/config.c`, `src/ds45pad.h`, `src/main.c`, `tests/test_kshim.c`, `tests/test_config.c`

**Interfaces:**
- `ds45_shim_kernel_t` gains `uint64_t exec_base, exec_bound;` (DEX 4.92: `0x800000000057C000`, `0x8000000000580000`). `alloc_desc` stays, used only as the source of the kernel TOC (`alloc_desc + 8`) for the flush/probe OPD; `alloc_pool` and the allocator CALL are removed. `spare_syscall`/`spare_slot_expect` stay (the flush and the probe borrow the spare slot).
- New debug-only probe: `int kshim_exec_probe(const ds45_shim_kernel_t *k, uint64_t *out);` — returns 0 and `*out` = the value the stub returned, or non-zero on a verify/plumbing failure. Config key `PROBE_EXEC` (`g_cfg.probe_exec`, VSH debug builds only, default 0); `src/main.c` runs the probe once after a kernel match when set and traces `exec_probe=<hex>`, installing nothing else.

- [ ] **Step 1: Installer tests** in `tests/test_kshim.c` (rework the allocation tests to fixed-base):
  - `test_install_uses_exec_base`: after install, the blob is at `exec_base` (event OPD at `exec_base + DS45_SHIM_OFF_EVENT`, data-block magic/version at `exec_base + DS45_SHIM_OFF_DATA`); the allocator syscall was never called (fake spare-call log records only the flush).
  - `test_install_refuses_nonzero_region`: a non-zero byte anywhere in `[exec_base, exec_base + DS45_SHIM_BLOB_LEN + KSHIM_FLUSH_OPD_LEN)` -> `KSHIM_FAILED`, image byte-identical.
  - `test_install_refuses_over_bound`: `exec_base + size > exec_bound` -> `KSHIM_FAILED`, nothing written.
  - `test_remove_zeroes_blob`: after install then `kshim_remove`, the blob region reads all zero (and the redirects/words are restored as before).
  - Keep `test_adopt_via_event_slot` (adoption now follows the event slot to the fixed base) and the hash/rollback/inline-gate tests.
- [ ] **Step 2: Probe tests** in `tests/test_kshim.c` and `tests/test_config.c`:
  - `test_exec_probe_stub`: over a fake LV2, `kshim_exec_probe` pokes the two-word leaf stub (`li r3,<magic>; blr`) at `exec_base`, the fake spare-call returns the stub's `li` immediate, and the probe reports that value then leaves `exec_base` zeroed.
  - `test_probe_exec_key`: `PROBE_EXEC` default 0; `= 1` -> 1; `= abc` -> 0.
- [ ] **Step 3: Run** `pwsh tests/run_tests.ps1 -Filter shim` and `-Filter config` - expected FAIL (undeclared `exec_base`, `kshim_exec_probe`).
- [ ] **Step 4: Implement:** add the row fields and DEX 4.92 values; in `kshim_install` replace the allocator borrow/call with `base = k->exec_base`, a zero-region verify and a `base + size <= k->exec_bound` bound-check before any write (keep the reloc copy, data block, `0x29B8` flush via the borrowed spare slot, redirects, journal, rollback, adoption); `kshim_remove` zeroes `[base, base + DS45_SHIM_BLOB_LEN + KSHIM_FLUSH_OPD_LEN)` after restoring; add `kshim_exec_probe` (stub `li r3,<magic>; blr` as one BE qword, flush 8 bytes, borrow the spare slot pointed at `{exec_base, kernel_toc}`, call, read back, zero the stub); add the `PROBE_EXEC` key and the `src/main.c` debug hook.
- [ ] **Step 5: Run** `pwsh tests/run_tests.ps1` and `python -I tools/test_shim_pack.py` - all PASS; build all four variants - four successes; game `.sprx` contain no `kshim`/`PROBE_EXEC` strings.
- [ ] **Step 6: Commit** - "feat: place the shim blob in executable kernel memory at a fixed base".

### Task 7: Debug tracing and input-hook isolation (spec Amendment 3)

Added 2026-11-02 after console step 1: the shim installed and ran (no freeze), but a DS3 (which must be untouched) broke on Bluetooth re-pair and the network dropped, while the XMB stayed alive. Disassembly shows the DS3 pass-through is correct, so this adds on-hardware tracing (surviving a network drop) and an isolation switch to localise the runtime cause. Runs **before** Task 5. Debug/VSH only; release behaviour unchanged except the data block grows (version 5).

**Files:**
- Modify: `shim/shim_abi.h`, `shim/shim.h`, `shim/shim.c`, `shim/entry.c`, `src/kshim.c`, `src/config.c`, `src/ds45pad.h`, `src/main.c`, `tests/test_shim.c`, `tests/test_kshim.c`, `tests/test_config.c`, `tests/test_ds3report.c` (ABI layout)

**Interfaces:**
- `ds45_shim_data_t` (shim_abi.h) gains, appended after `pid_reserved` (so every current offset is kept), `DS45_SHIM_VERSION` -> 5:

```c
	uint32_t dbg_event_calls, dbg_enable_calls, dbg_input_calls, dbg_input_padmiss; /* 208..220 */
	uint32_t dbg_event_ds, dbg_enable_ds, dbg_input_ds, dbg_last_pid;               /* 224..236 */
	uint64_t dbg_last_vtable;                                                       /* 240; size 248 */
```

- shim.c bumps the counters (lock-free, like the existing `stat_*`): `shim_event_complete` -> `dbg_event_calls` at entry (after the `!d` guard), `dbg_event_ds` when the pad is a DualSense; `shim_enable_complete` likewise with `dbg_enable_*`. New `void shim_input_note(ds45_shim_data_t *d, const uint8_t *pad);` (declared in shim.h): bumps `dbg_input_calls`; `pad==NULL` -> `dbg_input_padmiss++`; else records `dbg_last_vtable = ld64(pad)` and `dbg_last_pid = (pad[0x68]<<24)|(pad[0x69]<<16)|(pad[0x6A]<<8)|pad[0x6B]` (the big-endian VID||PID), and `dbg_input_ds++` when `shim_pad_is_dualsense(d,pad)`.
- `shim/entry.c` `shim_input`: after `pad = shim_pad_from_xport(...)`, call `shim_input_note(&ds45_shim_data, pad)` before `shim_pad_is_dualsense`. No other behaviour change.
- Config: `SHIM_INPUT_HOOK` key -> `g_cfg.shim_input_hook` (VSH builds only, default 1, any non-zero = 1), parsed like `SHIM_INLINE_PID`.
- `src/kshim.c` `kshim_install`: apply the input redirect (`0x37CE38`) only when `g_cfg.shim_input_hook`; otherwise install the helper (and inline, event, enable) but leave the input slot original and omit it from the journal. Rollback/remove are journal-driven and already handle the subset. `orig_input` is still filled in the data block.
- `src/main.c` (debug only): each housekeeping tick, when the shim is installed, read the data block's `dbg_*` and `stat_*` counters (via `ds45_lv2_read` at the install base) and append one line, e.g. `shimdbg ev=<..> en=<..> in=<..> inds=<..> padmiss=<..> xl=<..> lastpid=<..> lastvt=<..>`, so a network drop still leaves the trajectory on the HDD trace.

- [ ] **Step 1: Failing tests.** `tests/test_shim.c`: `test_dbg_counts_event` (a DualSense completion bumps `dbg_event_calls` and `dbg_event_ds`; a DS3 bumps `dbg_event_calls` only), `test_dbg_counts_enable` likewise, `test_input_note` (NULL pad -> `dbg_input_calls`+`dbg_input_padmiss`; a DS3 pad -> `dbg_input_calls`, `dbg_last_pid==0x054C0268`, `dbg_last_vtable==pad_vtable`, `dbg_input_ds==0`; a DualSense pad -> `dbg_input_ds==1`, `dbg_last_pid==0x054C0CE6`). `tests/test_kshim.c`: `test_input_hook_gated` (`shim_input_hook=0` -> `0x37CE38` untouched and absent from the journal, while event/enable/helper are written; `kshim_remove` restores the rest; `=1` -> input slot written as today). `tests/test_config.c`: `test_shim_input_hook_key` (default 1; `=0`->0; `=abc`->1 unchanged... i.e. junk leaves the default). `tests/test_ds3report.c`: extend the ABI layout test to the new offsets and `sizeof == 248`, version 5.
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter shim` / `-Filter config` - FAIL (undeclared fields/key).
- [ ] **Step 3: Implement** the fields (version 5), the counter bumps and `shim_input_note`, the `SHIM_INPUT_HOOK` gate in config and install, and the main.c debug trace line.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1`, `python -I tools/test_shim_pack.py`, and the four builds - all PASS; game `.sprx` contain no `kshim`/`SHIM_INPUT_HOOK`/`shimdbg` strings.
- [ ] **Step 5: Commit** - "debug: shim diagnostic counters, on-disk trace and input-hook isolation".

### Task 8: Hooks set their own TOC (spec Amendment 4) - THE DS3-break fix

Console isolation proved the start-up/enable/input hooks all run with the kernel's TOC in r2, not the blob's (the kernel's vtable dispatch never reloads r2; it passes the descriptor's env word in r11). So every hook reads `ds45_shim_data` through a wrong-r2 GOT load and `orig_*` becomes a wild pointer -> storm/freeze. Fix: each hook descriptor carries the blob TOC in its env word, and the hook entry is a trampoline `mr r2, r11; b <impl>`. Freestanding blob only; host build and release unchanged. This is the fix that should make the shim actually work.

**Files:** Modify: `shim/entry.c` (only). The packer, installer, shim.c, data block and tests do not change (the host tests call the pure functions; the packer already rebases the env word because `.TOC.@tocbase` is a TOC reloc).

**Interfaces / structure (entry.c):** keep the three wrapper bodies exactly as they are (decide via `shim_*_complete`/filter, else tail-call `orig_*`), but make their NAME build-dependent so the kernel-called entry can be a trampoline in the blob while the host keeps a plain C function:

```c
#ifdef DS45_HOST_TEST
#define SHIM_HOOK(n) n              /* host: the C function IS the hook (tests call it) */
#else
#define SHIM_HOOK(n) n##_impl       /* blob: C body is the impl; asm trampoline is the hook */
#endif
int SHIM_HOOK(shim_event)(void *drv, long status, long len) { /* current body */ }
int SHIM_HOOK(shim_enable)(void *drv, long status, long len) { /* current body */ }
int SHIM_HOOK(shim_input)(void *xport, uint8_t *dst, uint16_t len_in, int flag) { /* current body */ }

#ifndef DS45_HOST_TEST
/* The kernel's vtable dispatch (framework 0xCB5C0, input caller 0x2A4E00) loads
 * the descriptor's env word (opd+0x10) into r11 and the entry (opd+0) but never
 * r2, assuming the one kernel TOC. Our descriptors put the blob TOC in BOTH toc
 * and env, so r11 = blob TOC on entry; the trampoline adopts it into r2 before
 * the C body's TOC-relative data access. b <impl> keeps r3..r6 and LR, so the
 * impl returns straight to the kernel. The packer rebases all three descriptor
 * words (entry = ADDR64, toc and env = TOC base) to the load base. */
__asm__(
"\t.section .opd,\"aw\",@progbits\n"
"\t.p2align 3\n"
"\t.globl shim_event\n"  "shim_event:\n"  "\t.quad .Ltr_event, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_enable\n" "shim_enable:\n" "\t.quad .Ltr_enable, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.globl shim_input\n"  "shim_input:\n"  "\t.quad .Ltr_input, .TOC.@tocbase, .TOC.@tocbase\n"
"\t.text\n"
"\t.p2align 2\n"
".Ltr_event:\n"  "\tmr 2, 11\n" "\tb shim_event_impl\n"
".Ltr_enable:\n" "\tmr 2, 11\n" "\tb shim_enable_impl\n"
".Ltr_input:\n"  "\tmr 2, 11\n" "\tb shim_input_impl\n"
);
#endif
```

- [ ] **Step 1: Restructure entry.c** as above (bodies unchanged; the `SHIM_HOOK` macro; the `#ifndef DS45_HOST_TEST` asm block). Nothing else in the repo changes.
- [ ] **Step 2: Build** all four variants (`$env:CELL_SDK='C:/usr/local/cell'`). Expect the blob to pack (packer accepts the new `.opd`); `DS45_SHIM_RELOC_COUNT` rises by 3 (the three env words); game `.sprx` unchanged/clean.
- [ ] **Step 3: Disassemble the built blob to verify the ABI** (the only way to check PPC codegen off-console). Using `shim_blob.h` + capstone (scratchpad tools): (a) the descriptor at `DS45_SHIM_OFF_EVENT` has word[0] = a trampoline code offset, word[1] == word[2] == the blob TOC offset (`OFF_DATA`-ish `+0x8000` region), and all three offsets are in `ds45_shim_relocs`; (b) the trampoline at word[0] disassembles to `mr r2,r11` then `b` into `shim_event_impl`; (c) same for enable and input. Put the evidence in the report.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1` (host unaffected - the C bodies and pure functions are unchanged) and `python -I tools/test_shim_pack.py` - all PASS.
- [ ] **Step 5: Commit** - "fix: shim hooks adopt the blob TOC from r11 (kernel dispatch does not set r2)".

**Fix round 1 (review, 2026-11-03): internal branches + r2 restore.** The review and a controller objdump of `shim/shim.elf` found two more defects the trampoline alone does not solve; both are console-fatal and must be fixed before Task 5.

- **(a) Every internal `bl` in the blob branches to the callee's `.opd` DESCRIPTOR, not its code** (`.text` ends `0x15b4`, `.opd` is `0x15b8..0x17e0`; e.g. `bl 0x1690 <shim_event_complete>` targets a descriptor whose word0 is `0x00000000` = illegal). lld does not redirect REL24-against-an-`.opd`-symbol to the code entry, and the packer DROPs REL24. So the first helper call in any hook faults - the blob's internal calls have never worked. **Fix in `tools/shim_pack.py`:** for each REL24 (type 10) at offset O whose target symbol/section resolves into `.opd`, compute `D = sym_value + addend` (the descriptor), read `code = be64(image[D:D+8])` (descriptor word0 = code offset, link-time == absolute since the image links at 0), rewrite the branch's LI field `image[O] = (be32(image[O]) & 0xFC000003) | ((code - O) & 0x03FFFFFC)` (assert `code` is in `.text` and `code - O` fits signed 26 bits), and do NOT rebase O (PC-relative). A REL24 whose target is already in `.text` is left as-is. Add a guard: after packing, no REL24 target resolves into `.opd` (hard error). A `tools/test_shim_pack.py` case over the real ELF asserts the `*_complete`/helper call sites now point into `.text`.
- **(b) The hook must restore the kernel's r2 before returning** (the dispatch sites `0xCB5C0`/`0x2A4E00` do not `ld r2` after `bctrl`; they assume the callee preserves r2). **Fix:** the trampoline becomes a framed wrapper, not a tail jump. With (a) fixing REL24, the wrapper's `bl <impl>` reaches code, so per hook:

```
.Ltr_event:
	mflr 0
	std  0, 16(1)        ; LR -> caller frame
	stdu 1, -0x30(1)
	std  2, 0x20(1)      ; save the kernel's r2
	mr   2, 11           ; r2 = blob TOC (the descriptor env the kernel passed in r11)
	bl   shim_event_impl ; packer redirects this REL24 to impl code; impl runs on the blob TOC
	nop
	ld   2, 0x20(1)      ; restore the kernel's r2
	addi 1, 1, 0x30
	ld   0, 16(1)
	mtlr 0
	blr
```

	Keep the three `.opd` descriptors `{ .Ltr_x, .TOC.@tocbase, .TOC.@tocbase }` from the first attempt (env = blob TOC). The impl bodies are unchanged. Host build/release unaffected (asm still under `#ifndef DS45_HOST_TEST`).

- [ ] **Fix step 1: packer REL24->code** + the guard and the test_shim_pack case.
- [ ] **Fix step 2: the framed wrapper** (replaces the tail-`b`/`@toc`-jump trampoline).
- [ ] **Fix step 3: build all four + run the host suite + `python -I tools/test_shim_pack.py`;** then disassemble `src/shim_blob.h` and verify: every former `.opd`-targeting `bl` now lands in `.text`; each hook descriptor is `{tramp, blob TOC, blob TOC}` (all three rebased); each trampoline saves r2, sets r2 from r11, calls the impl's CODE, restores r2, returns. Evidence in the report.
- [ ] **Fix step 4: Commit** - "fix: packer retargets internal branches to code; hooks preserve the kernel TOC".

### Task 5: Console test steps (with the user, DEX 4.92)

**Files:**
- Modify: `tools/ps3_console_test.ps1` (add `-Mode startup`: deploy the debug sprx with a given config, reboot through webMAN, wait for the plugin loop, print the `kshim`/`cap_` trace lines; peeks at no more than 2-3 per second)

**Interfaces:**
- Consumes: Tasks 2-3 (and 4 if done); the debug capture keys `CAPTURE_PTR`, `CAPTURE_LV2`, `CAPTURE_LEN` (commit 5515b6f); the pad manager at `0x51B200` (slots `+0x18`, objects `+0x60`; with a USB DS3 in slot 0 the DualSense's pointer is `0x51B268`).
- Produces: ledger entries with each step's result; the recorded basic report bytes (Task 2 layout confirmed or corrected).

- [ ] **Step 0: Executability probe (no hooks).** Config `ENABLE=1`, `ENABLE_SHIM=0`, `PROBE_EXEC=1`. Expect the trace `exec_probe=0x5AED` (the stub's magic), proving `0x57C000` executes. Peek `0x57C000` reads back zero afterwards (the probe cleans up) and no hook slot changed. If the console hangs here, the region is not executable — stop; do not proceed to step 1. (This is the one new install-nothing call; it replaces trusting the heap.)
- [ ] **Step 1: Install, DualSense off.** Config `ENABLE=1`, `ENABLE_SHIM=1`, `SHIM_INLINE_PID=0`, `PROBE_EXEC=0`. Expect trace `kshim_installed`; peek `0x37CFF8` = `exec_base`+`OFF_EVENT` (`exec_base` = `0x57C000`), `0x37CFB0` = `exec_base`+`OFF_ENABLE`, `0x37CE38` = `exec_base`+`OFF_INPUT`, the blob's magic `DS45SHM1` at `0x57C000`+`OFF_DATA`, `0x29E2C8` = `4E80002068800CE6`, `0x37CFE8` unchanged `800000000033A208`. A DS3 over USB and over Bluetooth still drives the XMB. **Isolation (Amendment 3, DS3-break diagnosis):** on the first pass set `SHIM_INPUT_HOOK=0` so only the helper/event/enable install, and watch the `shimdbg ...` trace line while reconnecting the DS3 over Bluetooth; take a `DUMP_LV2=1` baseline right after install, before touching any controller. If the DS3 survives with the input hook off but breaks with `SHIM_INPUT_HOOK=1`, the input hook is the cause; if it breaks either way, the event/enable/helper path is. Read the result from the on-disk trace after recovery, never from a live peek burst.
- [ ] **Step 2: Completion.** Add `CAPTURE_PTR=0x51B268`, `CAPTURE_LV2=0x60`, `CAPTURE_LEN=128`; flip the DualSense's link-key type to `0x01` with the pad off; power it on. Pass: the capture shows the object staying for more than 5 s with +0x80/+0x82 = the template's versions, +0x88..+0x8F = `00 00 00 01 D5 00 44 53` and +0xD8 cleared; the data block's `stat_completed` = 1 and `stat_enabled` = 1; the pad manager's port table (`mgr+0xE0`, count `mgr+0xF0`) holds the DualSense's slot. If `stat_completed` = 1 but `stat_enabled` stays 0, the DualSense never answered `0xF4`: record it and stop (design question for the user). If no object forms, repeat with `SHIM_INLINE_PID=1` and record that the inline site is needed.
- [ ] **Step 3: Input.** Point the capture at the shared buffer (`CAPTURE_LV2=0x4326C8`, `CAPTURE_LEN=72`, no `CAPTURE_PTR`) and record one raw basic report before translation (temporarily `enabled=0` via a data-block poke, then back to 1). Compare with Task 2's layout; if it differs, fix the translator (Task 2 tests first) before continuing. Then check buttons, sticks and L2/R2 in the XMB and in one game.
- [ ] **Step 4: Together and teardown.** DS3 + DualSense: both have ports and independent input; then both DualSense pads (#1 and #2) together: different identities, both ported; DualSense off/on three times, each reconnect completes (`stat_completed` and `stat_enabled` increment); plugin unload restores `0x37CFF8`, `0x37CFB0`, `0x37CE38` and the helper word.
- [ ] **Step 5: Record** each step's result in the ledger; commit the script change - "test: start-up completion console mode".
