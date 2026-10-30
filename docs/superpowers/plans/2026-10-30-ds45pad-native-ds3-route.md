# DS45Pad Native DS3 Route (spec 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DualSense paired by USB cable connects over Bluetooth through the PS3's own DS3 driver and gets a native port, via a small in-kernel shim installed RAM-only on fingerprinted kernels.

**Architecture:** A freestanding PPC64 blob (`shim/`) holds a start-up wrapper, an input wrapper, a product-ID check and the DualSense-to-DS3 report translator. The plugin (`src/kshim.c`) allocates kernel memory, copies and relocates the blob, and applies a verified, journalled list of kernel patches (`src/kpatch.c`) taken from a fingerprinted table (`src/kshim_table.c`). USB pairing (`src/usbpair.c`) runs from the existing USB attach path.

**Tech Stack:** C99, Cell SDK 475.001 `ppu-lv2-gcc` (plugin and freestanding blob), Python 3 stdlib (blob packer), native clang host tests (`tests/run_tests.ps1`), webMAN MOD + PowerShell console scripts.

**Spec:** `docs/superpowers/specs/2026-10-30-ds45pad-native-ds3-route-design.md`

## Global Constraints

- No user prompts, menus or toasts anywhere in this feature; verification is passive; the debug build traces each step with `DS45_TRACE`/`ds45_boot_mark`.
- Kernel writes: RAM-only; only on a kernel whose fingerprint matches a `ds45_shim_table` entry; every target verified against its expected original before any write; every write read back; any failure rolls back all writes already made, in reverse order.
- Nothing Sony-proprietary in git: expected original kernel *code* is stored only as FNV-1a 64 hashes (`ds45_fnv1a64`) of the 8-aligned qword holding it; pointer values and addresses may be stored as values. The compiled blob and generated `src/shim_blob.h` are build outputs, git-ignored, never committed. Dumps stay in `tools/dumps/` (git-ignored).
- No degraded mode: if a fact, table entry or check is missing, the shim is not installed and every pad keeps today's behaviour (USB path, generic Bluetooth path).
- Identity: DualSense = VID `0x054C`, PID `0x0CE6`. DS3 PID `0x0268` and Move navigation PID `0x042F` must keep the Sony path.
- Redirects stay installed until reboot even if syscalls are disabled later; no syscall slot stays modified after install.
- DS3 input report produced by the shim: 49 bytes (`0x31`), report ID `0x01` at byte 0.
- Shim code never blocks, allocates, or loops unboundedly; bounds-checked against the actual read length.
- Builds: all four variants (`vsh`/`game` × release/debug) must build; the shim and pairing are VSH-only (absent from game builds). Host tests: `pwsh tests/run_tests.ps1` all PASS before every commit.
- Shell: PowerShell/Write/Edit for anything with backslashes; commit messages via a file (`git commit -F`); commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Never push.
- Console steps are RAM-only, use webMAN at `192.168.1.97` (DEX 4.92), and need the user present; never write to the console without the user's go-ahead in chat for that run.

## Review Focus

1. A DS3 (and any non-DualSense Sony device) connecting while the shim is installed must take exactly the original code path - wrappers tail-call the original for every non-`0x0CE6` connection, including when the connection lookup fails.
2. A DualSense report shorter than the translator's minimum, or with an ID other than `0x31`, must pass through byte-for-byte untouched (no partial translation).
3. Install interrupted at any step (alloc fails, a read-back mismatches, the second redirect's slot changed since verification) must leave the kernel exactly as before - including the temporary syscall slot.
4. Plugin unload while a pad is connected must restore every redirect, and a second install attempt on the same boot (plugin reloaded) must detect the already-installed blob/redirects and not stack a second copy.
5. Pairing a DualSense that is already paired to this PS3 must write nothing (no feature 0x0A, no registration call).

---

### Task 1: Settle the shim facts on DEX 4.92 (read-only research)

**Files:**
- Create: `docs/superpowers/notes/2026-10-30-ds3-route-facts.md`

**Interfaces:**
- Consumes: workspace-root tools `fn.py`, `dis.py`, `analysis/ff_492.json`, `analysis/lv2_code_492.bin`; dumps in `tools/dumps/dex492/` (incl. `ds3route/navpid_connected.bin`); webMAN read-only `/peek.lv2` only. Known map: spec "What is known"; ledger `.superpowers/sdd/2026-10-28-ds45pad-any-firmware/progress.md` lines about the DS3 driver.
- Produces: a fact sheet with exactly these fields (addresses as `0x80000000xxxxxxxx`, code identified by hash, never raw bytes). Every later task reads its values from here.
  - **F1 start-up hook:** the pointer slot (address) whose target runs the DS3 start-up for a Bluetooth Sony pad, its current value, the C prototype of the function it points to (arguments in r3..r10, return), how to get the connection's VID/PID from those arguments, and the exact condition that advances the pad object to running (return value, a field write, or a reply). Decision: what the wrapper must do for a DualSense to reach running.
  - **F2 input hook:** the pointer slot for transport method 4 (class `0x37CE18`), its current value, the prototype of `0x0C9D60` (buffer, length in/out), whether the result buffer starts with the report ID (`0x31`) or `0xA1`, how many bytes a DualSense report keeps (queue limit vs. the 0x41 buffer), and how to get VID/PID from its arguments.
  - **F3 GET_REPORT:** how to issue a Bluetooth HID GET_REPORT of feature `0x05` with `0x102750` (or another helper): prototype, report-type value, buffer/length, whether it may be called from the F1 context.
  - **F4 product-ID acceptance:** for site `0x11B5D8` (inline compare) and helper `0x29E2B4`: chosen patch kind (`WORD`, `BRANCH`) and new instruction, keeping `0x0268` and `0x042F`. If keeping `0x042F` is impossible at a site, say so explicitly: the controller must get the user's agreement before Task 7.
  - **F5 icache:** whether the CFW's syscall 7 poke makes written code coherent; if not, the LV2 routine (address, prototype) that does, callable through the spare syscall slot.
  - **F6 allocation:** syscall table address, a spare (unused) syscall number and its slot's current value, the LV2 allocator's descriptor address and prototype (size, pool), and the pool value to use for executable memory reachable by a 26-bit relative branch from the F4 sites.
  - **Unsatisfiable list:** any field that could not be settled, with the evidence.

- [ ] **Step 1: Settle F1-F6** with the analysis tools and dumps (read-only). Use console peeks only to confirm a live value; never poke.
- [ ] **Step 2: Write the fact sheet** with the fields above; state the evidence (function, offset, observed value) for each.
- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/notes/2026-10-30-ds3-route-facts.md
git commit -F <message file>   # "docs: DS3-route shim facts for DEX 4.92"
```

Gate: if the unsatisfiable list is non-empty, or F4 cannot keep `0x042F`, the controller stops and reports to the user before Task 6.

---

### Task 2: Bounded LV2 writes

**Files:**
- Modify: `src/lv2.h`, `src/lv2.c`, `tests/fakes.h`, `tests/fakes.c`
- Test: `tests/test_lv2.c`

**Interfaces:**
- Produces (in `lv2.h`):
  - `int ds45_lv2_write(uint64_t addr, uint64_t value);` - syscall 7 poke of one 8-aligned qword inside `[DS45_LV2_BASE, DS45_LV2_BOUND - 8]`, then reads it back with syscall 6. Returns 0 only if the read-back equals `value`; -1 without any syscall for a bad address.
- Fakes: syscall 7 writes into the fake image; `extern int fake_lv2_poke_count;` and `extern int fake_lv2_poke_fail_after;` (>= 0: that many pokes succeed, later pokes are silently dropped so read-back fails; -1 = never), both reset by `fake_reset_all()`.

- [ ] **Step 1: Write failing tests** in `tests/test_lv2.c`: `test_write_roundtrip` (write 0x1122334455667788 at `DS45_LV2_BASE+0x1000`, returns 0, `ds45_lv2_read` gives it back, `fake_lv2_poke_count == 1`); `test_write_rejects_bad_addr` (unaligned, below base, `DS45_LV2_BOUND - 4`: each -1 and `fake_lv2_poke_count == 0`); `test_write_readback_failure` (`fake_lv2_poke_fail_after = 0` -> returns -1).
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter lv2` - expected FAIL (undeclared `ds45_lv2_write`).
- [ ] **Step 3: Implement** `ds45_lv2_write` (`system_call_2(7, addr, value)`) and the fakes.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1` - expected all PASS.
- [ ] **Step 5: Commit** - "feat: bounded LV2 qword writes with read-back".

---

### Task 3: Journalled kernel patch engine

**Files:**
- Create: `src/kpatch.h`, `src/kpatch.c`
- Test: `tests/test_kpatch.c` (`/* SOURCES: lv2.c kpatch.c */`)

**Interfaces:**
- Consumes: `ds45_lv2_read`, `ds45_lv2_write`, `ds45_fnv1a64`.
- Produces (`kpatch.h`):

```c
typedef enum { DS45_KPATCH_PTR = 1, DS45_KPATCH_WORD, DS45_KPATCH_BRANCH } ds45_kpatch_kind_t;
typedef struct {
	ds45_kpatch_kind_t kind;
	uint64_t addr;      /* PTR: 8-aligned slot; WORD/BRANCH: 4-aligned instruction */
	uint64_t expect;    /* PTR: exact current value; WORD/BRANCH: FNV-1a 64 of the
	                     * 8-aligned qword holding the instruction */
	uint64_t value;     /* PTR: new pointer; WORD: new 32-bit instruction;
	                     * BRANCH: absolute branch target (encoded as `b target`) */
} ds45_kpatch_t;
#define DS45_KPATCH_MAX 8
typedef struct { int n; uint64_t qaddr[DS45_KPATCH_MAX]; uint64_t orig[DS45_KPATCH_MAX]; } ds45_kjournal_t;
int  kpatch_verify(const ds45_kpatch_t *p, int n);        /* 0 = every target as expected; no writes */
int  kpatch_apply(const ds45_kpatch_t *p, int n, ds45_kjournal_t *j); /* verify all, then write in order */
void kpatch_revert(ds45_kjournal_t *j);                   /* reverse order, read back; j->n = 0 */
int  kpatch_branch_word(uint64_t from, uint64_t to, uint32_t *word); /* 0 if within +-32 MiB */
```

`kpatch_apply` returns 0 on success; on any failure (verify, out-of-range branch, write/read-back) it reverts what it wrote and returns the 1-based index of the failing patch. `n > DS45_KPATCH_MAX` returns -1 with no reads.

- [ ] **Step 1: Write failing tests:** `test_verify_ptr_mismatch_writes_nothing`; `test_apply_ptr_and_word` (word at addr+4 replaced, other half of the qword kept, journal holds the original qword); `test_branch_encoding` (`kpatch_branch_word(0x...1000, 0x...2000)` gives `0x48001000`; backwards gives two's-complement; +-32 MiB +4 rejected); `test_apply_rollback_on_readback_failure` (`fake_lv2_poke_fail_after = 1` with 3 patches -> returns 2, image byte-identical to before); `test_revert_restores_all`; `test_too_many_patches`.
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter kpatch` - expected FAIL (missing header).
- [ ] **Step 3: Implement** `src/kpatch.c`.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1` - expected all PASS.
- [ ] **Step 5: Commit** - "feat: verified, journalled kernel patch engine".

---

### Task 4: Shim ABI and the DualSense-to-DS3 translator

**Files:**
- Create: `shim/shim_abi.h`, `shim/ds3_report.h`, `shim/ds3_report.c`, `src/sixaxis.h`, `src/sixaxis.c`, `src/ds45_state.h`, `src/ds_body.c`
- Modify: `src/dualsense.c`, `src/ds45pad.h` (includes `ds45_state.h`), `src/padldd.c` (use `sixaxis.c`), build source lists (`src/build.ps1`, `src/build.sh`, `src/Makefile`: add `sixaxis` and `ds_body` to every variant), test SOURCES lines that list `dualsense.c` (add `ds_body.c`)
- Test: `tests/test_ds3report.c` (`/* SOURCES: dualsense.c ds_body.c sixaxis.c util.c ../shim/ds3_report.c */`), existing tests stay green

**Interfaces:**
- Produces:
  - `src/sixaxis.h` (no SDK headers): `uint16_t sixaxis_from_accel(int16_t raw, int scale, int invert);` `uint16_t sixaxis_from_gyro(int16_t raw, int scale, int invert);` - moved unchanged from `padldd.c`'s static `sensor_from_*`; `DS45_SENSOR_CENTER` stays 512.
  - `src/ds_body.c` (includes only `ds45_state.h`, so the blob can build it): `int dualsense_parse_bt_body(const uint8_t *buf, int len, ds45_state_t *st);` - a Bluetooth `0x31` report without CRC check; requires `buf[0] == 0x31` and `len >= DS_BT_BODY_MIN` where `DS_BT_BODY_MIN` = `DS_OFF_BT` + the end of the last field parsed (status byte); `dualsense_parse` (both links) calls the shared field decoding in `ds_body.c` so it exists once; the `DS_*` layout constants move with it. `ds45_state_t` and this declaration must compile without `<cell/pad.h>`: move them into `src/ds45_state.h`, included by `ds45pad.h`.
  - `shim/shim_abi.h` (freestanding, shared by plugin and blob):

```c
#define DS45_SHIM_MAGIC   0x4453343553484D31ULL   /* "DS45SHM1" */
#define DS45_SHIM_VERSION 1
#define DS45_SHIM_CONNS   8
typedef struct {
	uint8_t deadzone;              /* STICK_DEADZONE, percent of half travel */
	uint8_t map_touchpad, map_mute;/* ds45_map_t values */
	uint8_t sixaxis, swap_xy, invert_x, invert_y, invert_z, invert_g;
	int16_t accel_scale, gyro_scale;
} ds45_shim_cfg_t;
typedef struct {
	uint64_t magic; uint32_t version; uint32_t enabled;
	uint64_t orig_startup, orig_input;   /* original descriptors (F1, F2) */
	uint64_t get_report;                 /* F3 helper descriptor */
	ds45_shim_cfg_t cfg;
	uint64_t conn_key[DS45_SHIM_CONNS];  /* per-connection identity (F1/F2) */
	uint8_t  conn_state[DS45_SHIM_CONNS];
	uint32_t stat_translated, stat_passed, stat_startups; /* debug counters */
} ds45_shim_data_t;
```

  - `shim/ds3_report.h`: `#define DS3_REPORT_LEN 49` and `int ds3_report_from_dualsense(const uint8_t *in, int in_len, const ds45_shim_cfg_t *cfg, uint8_t out[DS3_REPORT_LEN]);` - returns 0 and fills `out`, or -1 and leaves `out` untouched (wrong ID, too short).
- DS3 report layout (pinned by the kernel converter `0x2A4678`, byte offsets in `out`): `[0]=0x01`; `[2]`: Select 0x01, L3 0x02, R3 0x04, Start 0x08, Up 0x10, Right 0x20, Down 0x40, Left 0x80; `[3]`: L2 0x01, R2 0x02, L1 0x04, R1 0x08, Triangle 0x10, Circle 0x20, Cross 0x40, Square 0x80; `[4]`: PS 0x01; `[6..9]` = LX, LY, RX, RY; pressure `[14]` Up, `[15]` Right, `[16]` Down, `[17]` Left, `[18]` L2, `[19]` R2, `[20]` L1, `[21]` R1, `[22]` Triangle, `[23]` Circle, `[24]` Cross, `[25]` Square; sensors big-endian 16-bit at `[41]` X, `[43]` Y, `[45]` Z, `[47]` G; every other byte 0.
- Mapping: Create -> Select, Options -> Start, PS -> PS; touchpad click and Mute -> the DS3 bit of `cfg->map_touchpad` / `cfg->map_mute` (`DS45_MAP_NONE` = nothing); pressure: L2/R2 = analog triggers, others 0 or 255; sticks: within `deadzone` percent of 128 -> 128; motion via `sixaxis_from_*` with today's axis selection (`swap_xy`, inverts), or 512 for all four when `sixaxis == 0`.

- [ ] **Step 1: Write failing tests** in `tests/test_ds3report.c` using a synthetic `0x31` report built in the test from the DualSense BT layout (`DS_OFF_BT` + `DS_C_*` from `dualsense.c`'s documented offsets): `test_neutral` (sticks 0x80, no buttons -> out[0]=0x01, [2..4]=0, [6..9]=0x80, sensors 512 when `sixaxis=1` and raw 0); `test_buttons_each` (each of the 17 DS3 bits from its DualSense source, and its pressure byte 255); `test_triggers_analog` (l2=0x40 -> [18]=0x40, [3]&0x01 set from the digital L2 bit); `test_map_touchpad_to_ps`; `test_deadzone` (deadzone 10, lx=0x80+12 -> 0x80; +13 -> unchanged); `test_motion_scale` (accel x=8192 -> 512+113 at [41..42] big-endian); `test_rejects_wrong_id_and_short` (out unchanged, sentinel-filled); `test_fuzz` (10000 random buffers, random lengths 0..128, a deterministic LCG seed: never crashes, returns -1 for `in[0]!=0x31` or `len < DS_BT_BODY_MIN`).
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter ds3report` - expected FAIL (missing headers).
- [ ] **Step 3: Implement** the moves (`sixaxis.c`, `ds45_state.h`, `dualsense_parse_bt_body`) and `ds3_report.c`.
- [ ] **Step 4: Run** `pwsh tests/run_tests.ps1` - expected all PASS; build all four variants with `src/build.ps1` - expected OK.
- [ ] **Step 5: Commit** - "feat: DualSense to DS3 report translator for the kernel shim".

---

### Task 5: Shim blob build pipeline

**Files:**
- Create: `shim/entry.c` (exported descriptors placeholder: `shim_startup`, `shim_input`, `shim_pid_ok` that tail-call the originals / return the original answer), `shim/shim.ld`, `tools/shim_pack.py`, `tools/test_shim_pack.py`
- Modify: `src/build.ps1`, `src/build.sh`, `src/Makefile`, `.gitignore` (`src/shim_blob.h`, `shim/*.o`, `shim/*.elf`)

**Interfaces:**
- Produces `src/shim_blob.h` (generated each VSH build, before the plugin compiles):

```c
static const uint8_t  ds45_shim_blob[];            /* image linked at address 0 */
#define DS45_SHIM_BLOB_LEN  <n>                    /* multiple of 8 */
static const uint32_t ds45_shim_relocs[];          /* offsets of 64-bit absolute words to add the load base to */
#define DS45_SHIM_RELOC_COUNT <n>
#define DS45_SHIM_OFF_DATA     <off>               /* ds45_shim_data_t */
#define DS45_SHIM_OFF_STARTUP  <off>               /* descriptor of shim_startup */
#define DS45_SHIM_OFF_INPUT    <off>               /* descriptor of shim_input */
#define DS45_SHIM_OFF_PID_OK   <off>               /* entry (code) of shim_pid_ok */
```

- Blob compile: `ppu-lv2-gcc -m64 -O2 -ffreestanding -nostdlib -fno-builtin -fno-jump-tables -fno-stack-protector -mno-sdata` over `shim/entry.c shim/ds3_report.c src/sixaxis.c src/ds_body.c`, linked with `shim/shim.ld` (base 0, `.opd`/`.toc`/`.data`/`.bss` inside the image, `-Wl,-q` to keep relocations).
- `tools/shim_pack.py <elf> <out.h>` (stdlib only): parses ELF64 big-endian; accepts only `R_PPC64_ADDR64` (absolute words -> relocs list), TOC-relative and PC-relative relocations (dropped, position-independent); any other relocation type, any undefined symbol, or a TOC/data reference inside `shim_pid_ok` (it runs with the kernel's r2) is an error with a non-zero exit. Emits the header above.

- [ ] **Step 1: Write failing tests** `tools/test_shim_pack.py` (`python -m unittest tools.test_shim_pack`): a synthetic ELF64 BE built in the test with one `.text`, one `.opd` entry and one `R_PPC64_ADDR64` -> header lists one reloc at the right offset and the symbol offsets; an `R_PPC64_REL24` to an undefined symbol -> exit 1; a `R_PPC64_TOC16_HA` inside `shim_pid_ok` -> exit 1.
- [ ] **Step 2: Run** `python -m unittest tools.test_shim_pack` - expected FAIL (no module).
- [ ] **Step 3: Implement** `shim_pack.py`, `shim.ld`, `entry.c` placeholders and the build integration (VSH variants only; game builds neither build nor include the blob).
- [ ] **Step 4: Verify:** `python -m unittest tools.test_shim_pack` PASS; all four `src/build.ps1` variants build; `src/shim_blob.h` exists after a VSH build and `git status` does not show it; `pwsh tests/run_tests.ps1` all PASS.
- [ ] **Step 5: Commit** - "build: freestanding kernel shim blob and packer".

---

### Task 6: Shim wrappers (start-up, input, product ID)

**Files:**
- Create: `shim/shim.h`, `shim/shim.c`
- Modify: `shim/entry.c` (call into `shim.c`)
- Test: `tests/test_shim.c` (`/* SOURCES: ds_body.c sixaxis.c ../shim/ds3_report.c ../shim/shim.c */`)

**Interfaces:**
- Consumes: fact sheet F1-F4 (prototypes, identity lookup, running condition, GET_REPORT call); `ds3_report_from_dualsense`; `ds45_shim_data_t`.
- Produces (`shim/shim.h`), all taking the data block explicitly so they are host-testable; `entry.c` supplies it (from the descriptor's TOC/data-relative address) and the F1/F2 argument lists:
  - `int shim_is_dualsense(uint16_t vid, uint16_t pid);` - 1 only for 054C/0CE6.
  - `int shim_pid_ok(uint32_t vid, uint32_t pid);` - 1 for VID 054C with PID 0268, 042F or 0CE6, else 0 (the F4 helper's contract; leaf, no globals).
  - `<F2 return type> shim_input_filter(ds45_shim_data_t *d, uint16_t vid, uint16_t pid, uint8_t *buf, <F2 length type> *len);` - called after the original read: DualSense + `buf[0]==0x31` + translation OK -> `buf` = DS3 report, `*len = 49`, `stat_translated++`; otherwise untouched, `stat_passed++`.
  - `shim_startup_dualsense(ds45_shim_data_t *d, <F1 arguments>)` - issues the F3 GET_REPORT of feature 0x05 through `d->get_report`, then produces F1's running condition; `stat_startups++`. Non-DualSense: `entry.c` tail-calls `d->orig_startup` before reaching it.
- `d->enabled == 0` makes every wrapper behave exactly like the original.

- [ ] **Step 1: Write failing tests:** `test_pid_ok_table` (0268/042F/0CE6 -> 1; 05C4, 0CE6 with VID 054D -> 0); `test_input_ds3_untouched` (pid 0268, buffer byte-identical, len unchanged); `test_input_dualsense_translated` (synthetic 0x31 -> 49-byte DS3, len 49); `test_input_short_passthrough`; `test_input_disabled_passthrough`; `test_input_identity_unknown_passthrough` (the F2 identity lookup fails -> buffer untouched, original result returned); `test_startup_issues_feature_05` (fake `get_report` descriptor records report type/ID 0x05 and the result matches F1's running condition); `test_startup_get_report_fails` (the F1 failure path from the fact sheet: no crash, stat unchanged).
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter shim` - expected FAIL.
- [ ] **Step 3: Implement** `shim.c` and `entry.c`.
- [ ] **Step 4: Verify:** host tests all PASS; VSH builds re-pack the blob without packer errors.
- [ ] **Step 5: Commit** - "feat: kernel shim wrappers for DualSense start-up and input".

---

### Task 7: Installer, shim table, and plugin wiring

**Files:**
- Create: `src/kshim.h`, `src/kshim.c`, `src/kshim_table.c`
- Modify: `src/main.c` (install after `kernel_resolve`, VSH build, `mode != USB`; remove in `ds45_stop`), build source lists (VSH variants), `tests/fakes.c` (syscall dispatcher: a test-set handler for the spare syscall number)
- Test: `tests/test_kshim.c` (`/* SOURCES: lv2.c kpatch.c kshim.c kshim_table.c */`, `/* CFLAGS: -DDS45_DEBUG */`)

**Interfaces:**
- Consumes: Task 1 F1-F6 values; `kpatch_*`; `ds45_lv2_write/read`; `kernel_fingerprint_matches`; `src/shim_blob.h`; `g_cfg`.
- Produces (`kshim.h`):

```c
typedef struct {
	const char *name;
	uint64_t fp_addr, fp_hash;              /* same fingerprint scheme as ds45_kernel_t */
	uint64_t syscall_table; uint32_t spare_syscall; uint64_t spare_slot_expect;
	uint64_t alloc_desc; uint64_t alloc_pool;
	uint64_t flush_desc;                    /* 0 if F5 says the poke is coherent */
	uint64_t startup_slot, startup_expect;  /* F1 */
	uint64_t input_slot, input_expect;      /* F2 */
	uint64_t get_report_desc;               /* F3 */
	ds45_kpatch_t pid_patches[2];           /* F4, value of BRANCH = 0 -> shim_pid_ok */
} ds45_shim_kernel_t;
extern const ds45_shim_kernel_t ds45_shim_table[];
extern const int ds45_shim_table_count;
typedef enum { KSHIM_OFF = 0, KSHIM_INSTALLED, KSHIM_ALREADY, KSHIM_NO_ENTRY, KSHIM_FAILED } kshim_result_t;
kshim_result_t kshim_install(void);   /* traces the failing step in debug builds */
void kshim_remove(void);              /* restores redirects and patches; the blob stays */
```

- Install order (spec "Install"): match table -> `kpatch_verify` of the redirects, product-ID patches and the spare slot -> already installed? (startup/input slots already point at a blob whose data block has `DS45_SHIM_MAGIC`: adopt it, return `KSHIM_ALREADY`, write nothing) -> spare slot := `alloc_desc` (journalled), syscall `spare_syscall(len, alloc_pool)`, slot restored and read back -> range-check the allocation for the F4 branches -> copy blob qword by qword with relocations applied, read back -> data block (`orig_*`, `get_report`, `cfg` from `g_cfg`, `enabled = 1`) -> flush (F5) -> product-ID patches -> startup and input redirects. Any failure: `kpatch_revert`, spare slot restored, `KSHIM_FAILED`.
- The DEX 4.92 entry is the only table row; values from the fact sheet.

- [ ] **Step 1: Write failing tests** (fake image seeded with a synthetic table entry, fake syscall handler that "allocates" a fixed in-window address): `test_no_entry_writes_nothing`; `test_install_happy_path` (redirect slots point at blob descriptors, relocated words = base + offset, data block magic/orig values, spare slot back to its original value, `KSHIM_INSTALLED`); `test_install_fails_each_step` (alloc returns 0; alloc out of branch range; blob read-back mismatch via `fake_lv2_poke_fail_after`; input slot changed after verify via `fake_lv2_on_peek`) -> each `KSHIM_FAILED` and the image byte-identical to before, spare syscall slot included; `test_second_install_adopts` (`KSHIM_ALREADY`, zero pokes); `test_remove_restores`.
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter kshim` - expected FAIL.
- [ ] **Step 3: Implement** `kshim.c`, `kshim_table.c` (DEX 4.92 row), `main.c` wiring.
- [ ] **Step 4: Verify:** host tests all PASS; four variants build; `ds45pad_game*.sprx` contain no `kshim` strings (string scan as for the capture).
- [ ] **Step 5: Commit** - "feat: install the DS3-route shim on fingerprinted kernels".

---

### Task 8: Console stage - DS3 regression and a native DualSense (with the user)

**Files:**
- Modify: `tools/ps3_console_test.ps1` (new `-Mode native`)

**Interfaces:**
- Consumes: debug VSH build; webMAN; the boot trace (`kshim=`, `vsh_ports=`, shim debug counters traced once per rescan from the data block).
- `-Mode native`: deploy + reboot as today; wait for `kshim` in the trace; read-only check of the redirect slots; then, while DualSense #1 is off, set its link-key entry type 0x08 -> 0x01 (verified, read back; debug tooling only - Task 10 replaces it); ask the person at the console (script output and one webMAN popup, test tooling only) to switch it on and use it; follow the trace; afterwards restore type 0x08 when the pad is off.

- [ ] **Step 1: Add `-Mode native`** to the script; `pwsh -NoProfile -Command "[System.Management.Automation.PSParser]::Tokenize((Get-Content tools/ps3_console_test.ps1 -Raw), [ref]$null)"` parses.
- [ ] **Step 2: With the user's go-ahead, run stage 1:** DS3 over USB and over Bluetooth with the shim installed - expected: DS3 works, `stat_passed` grows, `stat_translated` stays 0.
- [ ] **Step 3: Run stage 2:** DualSense #1 flipped -> expected: a new port in `vsh_ports`, `stat_startups == 1`, `stat_translated` grows, sticks/buttons move in the trace.
- [ ] **Step 4: Run stage 5:** unload the plugin (webMAN) -> redirect slots read back as their original values.
- [ ] **Step 5: Record results** in the ledger; commit the script - "test: native-route console stage".

If stage 2 fails, stop and debug with superpowers:systematic-debugging before Task 9.

---

### Task 9: Settle the pairing facts (read-only research)

**Files:**
- Modify: `docs/superpowers/notes/2026-10-30-ds3-route-facts.md` (section "Pairing")

**Interfaces:**
- Produces:
  - **P1:** where the PS3's own Bluetooth address is available to the plugin (call or LV2 location) and its byte order.
  - **P2:** the registration call that persists a paired device (the Settings menu's own path, user space preferred): name/syscall number, arguments, whether it accepts type Sony (0x01) and a 16-byte link key, and how an existing entry for the same address is replaced.
  - **P3:** DualSense USB feature reports: `0x09` GET (length, pad address and current host address offsets and byte order) and `0x0A` SET (length, host address and link key offsets), from public sources (Linux `hid-playstation`, dualsensectl or equivalent), cited.
  - **P4:** the system random source available to a VSH plugin for the link key.
  - Decision per spec: P2 accepts Sony + key -> "direct"; otherwise "generic + type flip on boot".

- [ ] **Step 1: Settle P1-P4** (read-only analysis, public sources).
- [ ] **Step 2: Update the fact sheet**, including the decision.
- [ ] **Step 3: Commit** - "docs: pairing facts for the native DS3 route".

---

### Task 10: USB-cable pairing

**Files:**
- Create: `src/usbpair.h`, `src/usbpair.c`
- Modify: `src/usb.c` (call on DualSense attach, VSH build only), `src/kshim.c` (type flip on boot if P2 = "generic + type flip"), `tests/fakes.*` (fake control transfers for feature reports, fake registration call, fake random)
- Test: `tests/test_usbpair.c`

**Interfaces:**
- Consumes: P1-P4; existing `ds45_pad_t` control pipe (`c_pipe`).
- Produces (`usbpair.h`):

```c
typedef enum { PAIR_ALREADY = 0, PAIR_DONE, PAIR_FAILED } pair_result_t;
typedef struct {   /* injected so the logic is host-testable */
	int (*get_feature)(void *ctx, uint8_t id, uint8_t *buf, int len);
	int (*set_feature)(void *ctx, uint8_t id, const uint8_t *buf, int len);
	int (*host_addr)(uint8_t out[6]);
	int (*is_registered_sony)(const uint8_t pad[6], int *has_key);
	int (*register_pad)(const uint8_t pad[6], const uint8_t key[16]);
	int (*random_bytes)(uint8_t *out, int len);
	void *ctx;
} pair_io_t;
pair_result_t usbpair_run(const pair_io_t *io);
```

Steps inside `usbpair_run` exactly as the spec's Pairing 1-5; read back 0x09 after 0x0A and fail if the host address differs.

- [ ] **Step 1: Write failing tests:** `test_already_paired_writes_nothing` (no `set_feature`, no `register_pad`); `test_new_pad_pairs` (0x0A carries the host address and the random key at P3's offsets, `register_pad` gets the same key); `test_readback_mismatch_fails` (no `register_pad`); `test_get_feature_fails`; `test_register_fails` (`PAIR_FAILED`, traced).
- [ ] **Step 2: Run** `pwsh tests/run_tests.ps1 -Filter usbpair` - expected FAIL.
- [ ] **Step 3: Implement** `usbpair.c`, the real `pair_io_t` in `usb.c` (async `cellUsbdControlTransfer` with completion wait bounded to 1 s), and the type flip in `kshim.c` if the decision says so (verified, read back, only for entries whose address was paired by `usbpair`, recorded in `DS45_DIR "/paired.txt"`).
- [ ] **Step 4: Verify:** host tests all PASS; four variants build; game builds contain no `usbpair` strings.
- [ ] **Step 5: Commit** - "feat: pair a DualSense by USB cable like a DS3".

---

### Task 11: Console stage - pairing, persistence, three pads (with the user)

**Files:**
- Modify: `tools/ps3_console_test.ps1` (`-Mode pair`)

- [ ] **Step 1: Add `-Mode pair`** (deploy, reboot, follow `usbpair`/`kshim` trace lines; no link-key pokes).
- [ ] **Step 2: With the user's go-ahead, stage 3:** DualSense #1 unregistered in Settings first (user), then plugged in by USB, unplugged, PS pressed -> native port; reboot -> PS connects it natively again without plugging in.
- [ ] **Step 3: Stage 4:** DS3 plus DualSense #1 and #2 (each paired by cable) -> ports 1-3, each moving its own port.
- [ ] **Step 4: Re-plug a paired DualSense** -> trace shows `PAIR_ALREADY`, no write.
- [ ] **Step 5: Record results** in the ledger; commit the script - "test: pairing console stage".
