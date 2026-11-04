# DS3 Route Power-off Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the PS3 asks a Bluetooth DualSense (presented as a DualShock 3) to "Turn off the controller", the DualSense powers off. A real DS3 and every other device are untouched.

**Architecture:** A branch-and-link hook at the kernel's pad-close send (`bl 0x102750` at `0xCA9BC`, the feature report `0xF4` `42 08 00 00`) runs a pure decision function in the shim blob. For a DualSense pad in act mode it swaps the call's report id, payload and length for the DualSense power-off report (id `0x08`, 46-byte payload, CRC seed `0x53`) built in the shim's data block, then tail-calls the original wrapper. In observe mode it only counts and records, and three more observers sit on the kernel's link-teardown call sites. Stages: observe, act, final check.

**Tech Stack:** C (host tests with clang; freestanding PPC64 blob via clang + ld.lld + `tools/shim_pack.py`), PowerShell console scripts, webMAN MOD on the DEX 4.92 test console.

**Spec:** `docs/superpowers/specs/2026-11-04-ds45pad-ds3-route-poweroff-design.md`. Kernel facts: `docs/superpowers/notes/2026-11-04-ds3-route-poweroff-facts.md` (P1-P7, hash rows).

## Global Constraints

- DEX 4.92 only, fingerprint-gated; debug VSH builds only; RAM only; every kernel write verified by the FNV of its 8-aligned qword before writing, applied in the one all-or-nothing journalled transaction, reverted on unload. A mismatch at any applied site fails the whole install.
- Shim code rules: only loads and stores into its own objects and the call's objects, no locks, no allocation, no calls except the tail call to the original, no libc, no static data; built twice (host tests and the blob). The hook never blocks or fails a send: anything unexpected passes the call through with every register unchanged.
- DualSense power-off report (sourced, unverified until Task 1 and the console): feature report id `0x08`, 47 bytes in total, i.e. a **46-byte payload** after the id: `02`, 41 zero bytes, 4 CRC bytes (little-endian). CRC: reflected CRC-32 (poly `0xEDB88320`, init and final XOR `0xFFFFFFFF`) over the seed byte `0x53`, then `08`, then payload bytes 0-41.
- Hook contract at `0xCA9BC`: `r3` handle byte, `r4` = 3, `r5` = `0xF4`, `r6` payload (`obj+0x54`), `r7` = 4, `r8` cookie, `r9` object (kind at `obj+0x38`, pad at `obj+0x48`); the original is `0x102750`. A rewrite changes only `r5`, `r6`, `r7`. Teardown observers: `0x1174FC(address, flag)` at call sites `0xF4D94` (bl), `0xF722C` (tail b), `0xFDB44` (bl); installed in observe mode only; they change nothing.
- Hash rows (FNV-1a 64 of the 8-aligned qword): `0xCA9B8` `0x6D3B5A2728211E68`; `0xF4D90` `0x28861307B803CC8E`; `0xF7228` `0x2965949581F9EB53`; `0xFDB40` `0x10FC449574975856`. Task 1 re-verifies them.
- Patch budget: `DS45_KPATCH_MAX` becomes 20; the worst install is 17 entries; `DS45_SHIM_PID_SITES` 14 (sites 10-13 are the power-off sites: 10 the `0xCA9BC` call, 11-13 the teardown observers); `DS45_SHIM_VERSION` 12.
- Nothing Sony-proprietary in git: notes hold addresses, hashes and mnemonics only, never raw kernel bytes.
- Console safety: arm DualSenses by Bluetooth address only (never the Move motion controller's key `0006f799255b`); no peek bursts; no hard reset (hold the physical power button if the console hangs); every console stage ends with `tools/ps3_console_restore.ps1`; all verification passive; stop and report to the user if any pad misbehaves.
- Build with `src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (the `CELL_SDK` variable is POSIX-style). Host tests: `pwsh -File tests/run_tests.ps1` (regenerate `src/shim_blob.h` through the build before `test_kshim`). Test files start with `/* SOURCES: ... */`. Packer tests: `python -I -m unittest tools.test_shim_pack` from the repo root, or the way the earlier plan ran them.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; write messages to a file and use `git commit -F`. Do not create files with shell heredocs (backslashes are corrupted by the Bash tool); use the file tools.

## Review Focus

- A real DS3's (or any non-DualSense pad's) close request is never rewritten: result 0 and every output field untouched. Pinned in Task 4 (host) and Task 5 (stub review).
- Any deviation from the exact call shape (`r4` != 3, `r5` != `0xF4`, `r7` != 4, kind != 3, NULL object, NULL pad, disabled shim) passes through. Pinned in Task 4.
- Observe mode (`po_mode` 0) never rewrites, even for a DualSense, and the teardown observers exist only in observe mode. Pinned in Tasks 4 and 7.
- A repeated entry with identical arguments (the kernel's five retries) gives an identical rewrite, and two DualSense pads turned off at once write identical bytes. Pinned in Task 4.
- A mismatch at any applied power-off site fails the whole install with nothing written; unload and adoption revert all of them; the hook off installs exactly like version 11 (plus the new data block). Pinned in Tasks 3 and 7.

---

### Task 1: Gate - verify the facts and the DualSense report (read-only)

**Files:**
- Create: `docs/superpowers/notes/2026-11-04-ds3-route-poweroff-gate.md`

**Interfaces:**
- Produces: the settled values Tasks 4-7 use. **Gate:** any mismatch below stops the plan and is reported to the user; the spec is then reopened.

- [ ] **Step 0 (controller, before dispatching this task):** dispatch one research subagent (`general-purpose`, model `haiku`) with the question: "In the third-party `dualsensectl` source (and, if present, Linux `hid-playstation.c`, SDL's DualSense driver, pydualsense), what exactly is sent to power a Bluetooth DualSense off: report id, feature or output report, total length, which bytes are set, the CRC seed byte and the span it covers, and whether it is Bluetooth-only? Return source URLs and flag what you could not confirm." The decision hinges on the report being a **feature** report `0x08`, 47 bytes, `buf[1]` = 2, CRC seed `0x53` over the first 43 bytes. Put its answer into the gate note as section G0. If it differs (an output report, another id or length), stop and report.
- [ ] **Step 1:** With the scratchpad capstone helpers (run python with `-I` from outside the repo; `dis.py` in the workspace root shadows the stdlib) over `analysis/lv2_code_492.bin` and `tools/dumps/dex492/ds45pad_lv2.bin`, answer and record, each with the supporting addresses:
  - G1: the instruction at each of the four sites decodes as expected (`bl 0x102750` at `0xCA9BC`; `bl 0x1174FC` at `0xF4D94`; `b 0x1174FC` at `0xF722C`; `bl 0x1174FC` at `0xFDB44`), which half of its qword it is in, and the FNV of each qword equals the Global Constraints rows in both images.
  - G2: the registers live at `0xCA9BC` (r3-r9 as listed; which of r0, r11, r12, CR, CTR the instructions after the call and the entries `0x102750` and `0x1174FC` read before writing); `0x1174FC` takes `r3` as a value or a pointer.
  - G3: the width of the loads of `obj+0x38` (kind) and `obj+0x48` (pad) in `0xCA90C`.
  - G4: the retry path re-runs `0xCA90C` (so a rewritten call is rebuilt on each retry), and nothing reads the payload buffer `obj+0x54` after the send.
  - G5: `0xF8BD8` copies the payload into the packet buffer before returning and keeps no pointer to it (only the cookie and context), and a 48-byte wire packet (46 + id + header) fits both the kernel's 72-byte packet buffer and the control-channel transmit ring.
  - G6: the BT control syscall path (`0xF4D94`) and `0xF71E4` / `0xFDB44` are the only user-reachable callers of `0x1174FC` (re-run the caller index).
- [ ] **Step 2:** Write the note (addresses, hashes, mnemonics; no raw bytes) and a one-line verdict per G item. Run `tools/check_no_proprietary.ps1`.
- [ ] **Step 3:** Commit.

### Task 2: Pure power-off payload and a seeded CRC

**Files:**
- Modify: `shim/ds3_output.h`, `shim/ds3_output.c`
- Test: `tests/test_ds3output.c`

**Interfaces:**
- Produces: `#define DS_POWEROFF_REPORT_ID 0x08`, `#define DS_POWEROFF_PAYLOAD_LEN 46`, `uint32_t ds3_crc32_seeded(uint8_t seed, const uint8_t *p, int n)` (reflected CRC-32 of `seed` followed by `p[0..n)`; the output report's `crc_output` becomes a call to it with seed `0xA2`, same results) and `void ds_poweroff_payload(uint8_t out[DS_POWEROFF_PAYLOAD_LEN])` (writes all 46 bytes: `02`, 41 zeros, the CRC of seed `0x53` over `08` plus the first 42 payload bytes, little-endian; NULL is a no-op). No static data, bitwise CRC.

- [ ] **Step 1:** Tests in `tests/test_ds3output.c`: `test_poweroff_payload_golden` (length, byte 0 = `02`, bytes 1-41 zero, the CRC bytes equal an independent reference loop written in the test over the 43 bytes `53 08 02 00...`), `test_poweroff_payload_null_is_noop`, `test_crc_seeded_matches_reference` (several seeds and lengths, including 0), `test_output_report_unchanged_by_refactor` (the existing golden output-report bytes still match for all four ports).
- [ ] **Step 2:** Run `pwsh -File tests/run_tests.ps1 -Filter ds3output`; expect a build failure.
- [ ] **Step 3:** Implement. Keep the CRC loop shape of `crc_output` (the `-(int32_t)(crc & 1)` mask, no table).
- [ ] **Step 4:** Run the filter again; expect all pass. Run the full host suite.
- [ ] **Step 5:** Commit.

### Task 3: Data block v12 and patch-table plumbing (no new behaviour)

**Files:**
- Modify: `shim/shim_abi.h`, `src/kpatch.h`, `src/kshim.h`, `src/kshim.c`, `shim/entry.c` (the `SHIM_OFF_ORIG_OUTPUT` constant only)
- Test: `tests/test_ds3report.c` (`test_abi_layout`), `tests/test_kpatch.c`, `tests/test_kshim.c`

**Interfaces:**
- Produces: `DS45_SHIM_VERSION 12`; `DS45_SHIM_PID_SITES 14` (so `pid_orig` grows by 32 bytes and every later field moves by 32; `orig_output` becomes offset 360); `DS45_KPATCH_MAX 20`; in `kshim.h` `#define DS45_SHIM_POWEROFF_PATCHES 4`, row fields `ds45_kpatch_t poweroff_patches[DS45_SHIM_POWEROFF_PATCHES]`, `uint64_t orig_poweroff` (code address `0x102750`), `uint64_t orig_teardown` (code address `0x1174FC`). Fields appended to `ds45_shim_data_t` after `dbg_in_raw`, in this order: `uint64_t orig_poweroff, orig_teardown; uint32_t po_mode, po_seq; uint32_t dbg_po_calls, dbg_po_ds, dbg_po_rewritten, dbg_po_passed; uint32_t dbg_td_calls, dbg_td_rsvd; uint8_t po_buf[48]; uint8_t po_ring[8][16];` (224 bytes; the struct is 1208 bytes, a multiple of 8). The 16-byte ring record layout is documented in the header comment: bytes 0-3 sequence (big-endian), byte 4 site (1 = close send, 2 = teardown), then for site 1: byte 5 DualSense flag, 6 handle, 7 `r4`, 8 report id, 9 length, 10 kind (`0xFF` if unreadable), 11 zero, 12-15 the first four payload bytes (zero-padded); for site 2: byte 5 zero, 6 flag, 7 zero, 8-11 low 32 bits of LR, 12-15 low 32 bits of the address argument. In `kshim.c`: site numbers `SITE_PO0 10` .. `SITE_PO0 + 3`; `pid_site()` maps sites 10-13 to `poweroff_patches[0..3]`; a helper `site_is_recv(i)` (`SITE_RECV0 <= i < SITE_PO0`) replaces every `i >= SITE_RECV0` test (in `site_selected`, `adopt_pid_sites`, the `nfront` loop of `kshim_install_from`) and the loop bound of `adopt_recv_unsafe` becomes `SITE_PO0`; `PID_ORDER` is `0, 1, 2, SITE_BUF, SITE_BUF1, SITE_OUT_CALL, SITE_OUT_CAP, SITE_PO0..SITE_PO0+3, SITE_RECV0..SITE_RECV0+2`; `site_selected` returns 0 for sites 10-13 and `site_restorable` for them requires only a populated row (Task 7 adds the real rules); `KSHIM_FINAL_MAX` is 17 (`kshim_final_is_thirteen` becomes `kshim_final_is_seventeen`, the other compile-time checks follow).

- [ ] **Step 1:** Update the existing assertions that pin the old layout and write the failing ones first: `test_abi_layout` (version 12, `DS45_SHIM_PID_SITES == 14`, `sizeof(pid_orig) == 8 * 14`, every field after `pid_orig` moved by 32, the new field offsets 8-aligned, `sizeof(ds45_shim_data_t) == 1208`, `sizeof(po_ring) == 128`), `test_kpatch.c` (`DS45_KPATCH_MAX == 20`, the limit tests and the `j.n == 16` check follow the constant), and in `test_kshim.c` the `DS45_SHIM_VERSION == 11` and `PID_SITES == 10` checks become 12 and 14, `test_blob_tail_is_the_data_block` still holds, plus the new `test_poweroff_sites_unselected_by_default` (every existing fixture install leaves `pid_applied` bits 10-13 clear, the new data-block fields zero, and journals the same number of entries as before).
- [ ] **Step 2:** Run the host tests; expect failures.
- [ ] **Step 3:** Implement the struct, constants and `kshim.c` plumbing. In `entry.c` set `SHIM_OFF_ORIG_OUTPUT` to the new offset (360); the existing `shim_off_orig_output_matches_abi` typedef must hold. Rebuild the blob with `src/build.ps1` so `src/shim_blob.h` matches the new layout.
- [ ] **Step 4:** Run the full host suite; expect all pass with no behaviour change (the `g_ord_checks == 13` and journal-size checks are unchanged because no power-off site is selected).
- [ ] **Step 5:** Commit.

### Task 4: Hook decision, ring and teardown note (pure, host-tested)

**Files:**
- Modify: `shim/shim.h`, `shim/shim.c`
- Test: `tests/test_shim.c`

**Interfaces:**
- Consumes: Task 2 (`ds_poweroff_payload`, `DS_POWEROFF_REPORT_ID`, `DS_POWEROFF_PAYLOAD_LEN`), Task 3 fields, `shim_pad_is_dualsense`.
- Produces (in `shim.h`): `typedef struct { uint64_t handle, type, rid, payload, len, cookie, obj; uint64_t o_rid, o_buf, o_len; } ds45_po_call_t;` (the in-registers `r3`..`r9` and the three replacement values), `#define SHIM_PO_OBJ_KIND 0x38`, `#define SHIM_PO_OBJ_PAD 0x48`, `#define SHIM_PO_KIND_CLOSE 3` (kind read with the width Task 1 recorded), `int shim_poweroff_filter(ds45_shim_data_t *d, ds45_po_call_t *c)`, `void shim_teardown_note(ds45_shim_data_t *d, uint64_t addr, uint64_t flag, uint64_t lr)`.
  - `shim_poweroff_filter`: `d` or `c` NULL: return 0, write nothing. Otherwise `dbg_po_calls++`; read kind and pad from `c->obj` only when it is non-NULL (kind `0xFF` and no pad otherwise); `is_ds = shim_pad_is_dualsense(d, pad)` (`dbg_po_ds++` when set); append a site-1 ring record (`po_seq++` first, slot `(seq - 1) & 7`, layout in `shim_abi.h`; a disabled shim records nothing and returns 0). Rewrite only when `d->enabled`, `d->po_mode == 1`, `c->type == 3`, `c->rid == 0xF4`, `c->len == 4`, kind == `SHIM_PO_KIND_CLOSE` and `is_ds`: fill `d->po_buf` with `ds_poweroff_payload`, set `c->o_rid = 0x08`, `c->o_buf = (uint64_t)(uintptr_t)d->po_buf`, `c->o_len = 46`, `dbg_po_rewritten++`, return 1. Every other path: `dbg_po_passed++`, return 0, `o_*` untouched.
  - `shim_teardown_note`: `d` NULL or disabled: nothing. Otherwise `dbg_td_calls++` and a site-2 ring record. Never changes any argument (it returns nothing).

- [ ] **Step 1:** Tests in `tests/test_shim.c`, following `test_output_*`: `test_poweroff_rewrites_dualsense` (exact `o_rid`/`o_buf`/`o_len`, `po_buf` equals `ds_poweroff_payload`, counters), `test_poweroff_ds3_untouched` (Sony pad, PID `0x0268`: 0, `o_*` untouched, `po_buf` zero), `test_poweroff_wrong_shape_passes` (each of `type` 2, `rid` `0xF3`, `len` 3 and 5, kind 4, NULL `obj`, `obj` whose pad pointer is NULL, `enabled` 0: return 0), `test_poweroff_observe_mode_never_rewrites` (`po_mode` 0 and 2: 0, ring and counters still recorded), `test_poweroff_retry_is_idempotent` (five identical calls: identical `o_*` and `po_buf`; `po_seq` 5), `test_poweroff_two_pads_same_bytes` (two DualSense objects, interleaved calls, `po_buf` identical), `test_poweroff_ring_records` (golden 16-byte records for a close send and a teardown, `po_seq` wraps the 8 slots, big-endian sequence), `test_teardown_note_counts_and_disabled`, `test_glue_poweroff` (the `SHIM_HOOK` names, like `test_glue_output`).
- [ ] **Step 2:** Run `pwsh -File tests/run_tests.ps1 -Filter shim`; expect failures.
- [ ] **Step 3:** Implement with plain bounded byte loops, no libc, no static data; encode the sequence and addresses byte by byte so the ring layout is the same on the host and the blob.
- [ ] **Step 4:** Run the full host suite; expect all pass.
- [ ] **Step 5:** Commit.

### Task 5: Blob stubs and packer exports

**Files:**
- Modify: `shim/entry.c`, `tools/shim_pack.py`, `tools/test_shim_pack.py`
- Test: `tests/test_shim.c` (glue), `tools/test_shim_pack.py`

**Interfaces:**
- Consumes: Task 4 functions, `SHIM_OFF_ORIG_*` constants (asserted with `__builtin_offsetof` like `SHIM_OFF_ORIG_OUTPUT`).
- Produces: in `entry.c` `int SHIM_HOOK(shim_poweroff)(ds45_po_call_t *c)` (calls `shim_poweroff_filter(&ds45_shim_data, c)`) and `void SHIM_HOOK(shim_teardown)(uint64_t addr, uint64_t flag, uint64_t lr)`; blob entries `shim_poweroff` and `shim_teardown` (descriptor in `.opd` for the packer, code reached by `bl` / `b`, never through the descriptor); `tools/shim_pack.py` exports `DS45_SHIM_OFF_POWEROFF` and `DS45_SHIM_OFF_TEARDOWN` (code addresses) and fails if either is missing or not in an executable section.
  - `shim_poweroff` stub: same technique as `shim_output` (adopt the blob TOC from a rebased literal via `bcl 20,31`, kernel `r2` saved and restored, LR saved and restored, tail call by `bctr`). Frame 0xD0: parameter area `0x30..0x6F`, the `ds45_po_call_t` at `0x70..0xBF` (the stub stores `r3`..`r9` directly into `handle, type, rid, payload, len, cookie, obj`; `o_*` are written by the body), saved LR `0xC0`, saved `r2` `0xC8`. It passes `r3 = &struct`, calls `shim_poweroff_impl`, loads `orig_poweroff` into CTR while `r2` is still the blob TOC, restores the kernel `r2`, `r3`, `r4`, `r8`, `r9`, then `r5..r7` from `o_rid, o_buf, o_len` if the result is non-zero or from `rid, payload, len` otherwise, restores LR, pops the frame, `bctr`.
  - `shim_teardown` stub: frame 0xC0, saved LR `0x70`, saved `r2` `0x78`, `r3`..`r10` at `0x80..0xBF`; calls `shim_teardown_impl(r3, r4, LR)`; then restores everything and `bctr`s to `orig_teardown`. It changes no argument.

- [ ] **Step 1:** Tests: in `tools/test_shim_pack.py` extend the ElfBuilder fixture with the two symbols and add `test_poweroff_and_teardown_exports` (both constants appear in the header, code addresses 4-aligned, the TOC literals are rebased like the `shim_output` one), and extend the required-symbol rename test list; `test_glue_poweroff` from Task 4 covers the `SHIM_HOOK` names.
- [ ] **Step 2:** Run the packer tests; expect failures.
- [ ] **Step 3:** Implement the stubs (follow `.Ltr_output`; the comment block above it is the contract to mirror) and the packer export. Build with `src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell`.
- [ ] **Step 4:** Disassemble the two built stubs from the blob with the scratchpad capstone helpers and put in the task report the full instruction listing of both, with a line per touched register; confirm no register outside `r0, r2 (restored), r3-r10, r11, r12, CTR, LR (restored), CR0, CR7` is written and every argument register is restored on the pass-through path. Run the packer tests and the full host suite.
- [ ] **Step 5:** Commit.

### Task 6: Config keys and README

**Files:**
- Modify: `src/ds45pad.h`, `src/config.c`, `README.md`
- Test: `tests/test_config.c`

**Interfaces:**
- Produces: `int shim_poweroff_hook` (default 1) and `int shim_poweroff_mode` (default 0, clamped to 0..1), VSH-only keys `SHIM_POWEROFF_HOOK` and `SHIM_POWEROFF_MODE` (a non-number keeps the default, last wins, like `SHIM_OUTPUT_*`), boot marks `cfg_shim_poweroff_hook` / `cfg_shim_poweroff_mode`; README rows in the config table and a short "Turn off the controller" note (observe mode records only; act mode powers a DualSense off; DEX 4.92 debug builds only).

- [ ] **Step 1:** Tests modelled on the `SHIM_OUTPUT_*` ones: defaults, `0`/`1`/other numbers for the hook, mode clamp (`-3` -> 0, `7` -> 1), junk keeps the default, last wins, ignored in the game variant.
- [ ] **Step 2:** Run `pwsh -File tests/run_tests.ps1 -Filter config`; expect failures.
- [ ] **Step 3:** Implement and document.
- [ ] **Step 4:** Run the full host suite.
- [ ] **Step 5:** Commit.

### Task 7: Installer rows, selection and adoption

**Files:**
- Modify: `src/kshim_table.c`, `src/kshim.c`
- Test: `tests/test_kshim.c`

**Interfaces:**
- Consumes: Task 3 plumbing, Task 5 exports (`DS45_SHIM_OFF_POWEROFF`, `DS45_SHIM_OFF_TEARDOWN`), Task 6 config fields, the hash rows.
- Produces: the DEX 4.92 row: `poweroff_patches[0]` BRANCHL at `0x80000000000CA9BC` (expect `0x6D3B5A2728211E68`), `[1]` BRANCHL at `0x80000000000F4D94` (expect `0x28861307B803CC8E`), `[2]` BRANCH at `0x80000000000F722C` (expect `0x2965949581F9EB53`), `[3]` BRANCHL at `0x80000000000FDB44` (expect `0x10FC449574975856`); `orig_poweroff = 0x8000000000102750`, `orig_teardown = 0x80000000001174FC`. `pid_value` dispatches BRANCH and BRANCHL on the site: site 10 -> `base + DS45_SHIM_OFF_POWEROFF`, sites 11-13 -> `base + DS45_SHIM_OFF_TEARDOWN`, the existing sites unchanged. `site_selected`: site 10 when `shim_poweroff_hook` is set and `k->orig_poweroff != 0`; sites 11-13 additionally only when the data block's mode is observe (`shim_poweroff_mode` 0, which includes any out-of-range value) and `k->orig_teardown != 0`. `site_restorable` for sites 10-13 requires a populated row and the matching sender address. `build_data_block` writes `orig_poweroff`, `orig_teardown` and `po_mode` (the mode, 0 or 1). Apply order: after the output call and cap word, before the event redirect (PID_ORDER from Task 3). The unload and adoption revert them with the rest of the journal.

- [ ] **Step 1:** Tests, with a `plant_poweroff_rows()` fixture next to `plant_output_rows()` (made-up qwords, distinct addresses, the BRANCH row for site 12 in the right half): `test_poweroff_sites_gated` (hook off; mode 0 selects sites 10-13, mode 1 selects site 10 only; row without `orig_poweroff` or without `orig_teardown`; out-of-range mode is observe; `pid_applied` bits, `pid_orig` entries and journal/poke counts for each case; unload restores the snapshot), `test_poweroff_site_index_mapping` (each row's own qword changes to its own value; tamper with row k refuses the install with nothing written; in act mode tampering with rows 1-3 is never noticed), `test_poweroff_branch_targets` (site 10 aims at the power-off entry, sites 11-13 at the teardown entry, encodings worked out independently, and an adopted install at a second base restores them), `test_poweroff_with_all_hooks_fits_journal` (output mode 1, inline PID, power-off mode 0: 17 journalled entries, rollback at every step restores the image), `test_poweroff_adopt_v12` (adoption restores the applied power-off sites; a v11 block is not adopted), `test_poweroff_data_block_fields` (`po_mode`, `orig_poweroff`, `orig_teardown`), and extend the built-in-row test and the pristine-image row test with the four new rows and the two sender addresses.
- [ ] **Step 2:** Run the host tests (rebuild the blob first); expect failures.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the full host suite and the four SDK builds (`vsh` and `game`, with and without `-Debug`); run `tools/check_no_proprietary.ps1`.
- [ ] **Step 5:** Commit.

### Task 8: Trace read-back

**Files:**
- Modify: `src/main.c` (debug section next to `shim_dbg_trace`), `tools/ps3_console_trace.ps1`

**Interfaces:**
- Produces: a `podbg` line (the counters `dbg_po_calls`, `dbg_po_ds`, `dbg_po_rewritten`, `dbg_po_passed`, `dbg_td_calls`, `po_mode` as `0x` + 8 hex digits, written when any of them changed) and one `poevt` line per ring record newer than the last printed (`poevt seq=0x........ ` then the 32 hex digits of the record, oldest first), appended to `DS45_BOOT_FILE`; `ps3_console_trace.ps1` also prints the last `podbg` line and every `poevt` line.

- [ ] **Step 1:** Implement in the same style as `shim_dbg_inraw`; no host test (debug only, `DS45_DEBUG`). Keep each line under the buffers used by the neighbours.
- [ ] **Step 2:** Build the four SDK variants; expect them to pass. Run the full host suite.
- [ ] **Step 3:** Commit.

### Task 9: Console stage 1, observe (controller with the user; not delegated)

**Files:**
- Create: `docs/superpowers/notes/2026-11-04-ds3-route-poweroff-console.md`

- [ ] **Step 1:** Build the debug VSH plugin; deploy with `tools/ps3_console_deploy.ps1 -Ip 192.168.1.97 -Dest <absolute path under tools/dumps/dex492/poweroff1> -Config` with the usual keys plus `SHIM_POWEROFF_HOOK = 1`, `SHIM_POWEROFF_MODE = 0` (call the script in-process with an absolute `-Dest` and `[Environment]::CurrentDirectory` set to the repo root). Confirm the four sites read back patched (`bl` / `b` into the blob) and the first `podbg` line appears.
- [ ] **Step 2:** Arm the DualSenses by Bluetooth address (`tools/ps3_console_linkkeys.ps1 -Arm -Bd`). Ask the user to connect the DS3 and one DualSense. Ask the user to choose "Turn off the controller" for the **DS3** and tell me when done; read the trace (`ps3_console_trace.ps1`). Ask the user to reconnect it, then do the same for the **DualSense**; read the trace again.
- [ ] **Step 3:** Record in the note what each action produced: `poevt` records (site, kind, report id, length, payload, DualSense flag), whether any teardown record appeared and its flag, order and counts, what the user saw. Classify per the spec: A (close send fires for both, no teardown), B (close send plus teardown, also for the DS3), C (close send does not fire). **Gate:** C, or any freeze or pad regression, stops the plan for the user (spec amendment). Restore with `tools/ps3_console_restore.ps1` and confirm the hooks are original. Commit the note.

### Task 10: Console stage 2, act, default and docs (controller with the user; not delegated)

**Files:**
- Modify: `src/config.c` (default `shim_poweroff_mode` 1), `tests/test_config.c`, `README.md`, the spec's status line, the console note

- [ ] **Step 1:** Deploy with `SHIM_POWEROFF_MODE = 1`; only the close-send site must read back patched. With the DS3 and one DualSense connected, ask the user to choose "Turn off the controller" for the DualSense; ask what the pad did (light bar out, link gone) and read the trace. Ask the user to bring it back with the PS button and confirm the same controller number, working input, rumble and LEDs. Repeat for the DS3 (still works as before).
- [ ] **Step 2:** Pass: the DualSense powers off, comes back with the same number, no freeze, the DS3 unaffected. **Gate:** if the DualSense only disconnects, stop and report: the next step is a spec amendment (link-teardown fallback), not an extension of this plan. Restore.
- [ ] **Step 3:** Final check of the reviewed build: two DualSense and the DS3, each turned off in turn and reconnected; colours and player LEDs still right. Restore.
- [ ] **Step 4:** Change the default mode to 1 with its test, update the README, set the spec status to implemented, run all host tests and the four builds. Push, open the PR with the verification list and everything not verified, and bind it.
