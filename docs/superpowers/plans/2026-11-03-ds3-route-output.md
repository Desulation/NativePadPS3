# DS3 Route Output (rumble, player LEDs, light bar) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DualSense that the PS3 treats as a DualShock 3 gets real rumble, player LEDs and a light bar colour per controller number, by rewriting the DS3 output report the kernel already sends into a DualSense `0x31` report.

**Architecture:** A branch-and-link hook at the kernel's Bluetooth output call (`0x2A2DE8`) runs a pure decision function in the shim blob. For a DualSense pad it swaps the call's report id, buffer and length for a 78-byte DualSense report built in the shim's data block, then tail-calls the original sender. The sender's 64-byte payload cap is raised. Stages: observe, probe, full.

**Tech Stack:** C (host tests with clang; freestanding PPC64 blob via clang + ld.lld + `tools/shim_pack.py`), PowerShell console scripts, webMAN MOD on the DEX 4.92 test console.

**Spec:** `docs/superpowers/specs/2026-11-03-ds45pad-ds3-route-output-design.md`

## Global Constraints

- DEX 4.92 only, fingerprint-gated; debug VSH builds only; RAM only; every kernel write verified by the FNV of its 8-aligned qword before writing, applied in the one all-or-nothing transaction, reverted on unload.
- Shim code rules: only loads and stores into its own objects and the call's buffer, no locks, no allocation, no calls except the tail call to the original, no libc, no static data; built twice (host tests and the blob). The hook never blocks a send: anything unexpected passes the call through unchanged.
- DualSense report: `0x31`, 78 bytes, sequence nibble from a wrapping counter, tag `0x10`, CRC-32 (reflected, init `0xFFFFFFFF`, final XOR) over `0xA2` then bytes 0-73, little-endian at 74-77. The kernel sender takes the report id separately, so the payload is 77 bytes; the cap at `0xF8D74` becomes `0x4D` (word `0x2B9F004D`).
- Spec Amendment 1: the shared packet buffer `0x4326C8` is only 72 bytes, so the two TOC slots that point to it (`0x80000000_00372650` = buffer, `0x80000000_00372660` = buffer + 1) are redirected by pointer patches to a 96-byte `pktbuf` in the data block. The DS3 payload the kernel builds is 48 bytes (id excluded): rumble bytes 1-4, LED bitmap byte 9. Facts note: `docs/superpowers/notes/2026-11-03-ds3-route-output-facts.md` (hashes, expected values, register contract).
- Nothing Sony-proprietary in git: notes hold addresses, hashes and mnemonics only, never raw kernel bytes.
- Console safety: arm DualSenses by Bluetooth address only (never the Move motion controller's key); no peek bursts; every console stage ends with `tools/ps3_console_restore.ps1`; stop and report to the user if any pad misbehaves.
- Build with `src/build.ps1 -Variant vsh -Debug -Sdk C:\usr\local\cell` (the `CELL_SDK` variable is POSIX-style). Host tests: `pwsh -File tests/run_tests.ps1` (regenerate `src/shim_blob.h` through the build before `test_kshim`). Test files start with `/* SOURCES: ... */`.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; write messages to a file and use `git commit -F`.

## Review Focus

- A real DS3's (or any non-DualSense pad's) output call is never rewritten: arguments identical on return. Pinned in Task 4 and Task 8.
- A DualSense pad whose payload is empty, short, long or not the captured shape passes through; the hook never errors. Pinned in Task 7 and Task 8.
- NULL buffer, zero length, length over 64 and a disabled shim pass through. Pinned in Task 4.
- Two DualSense pads and a reconnect: each pad's colour and LEDs come from its own LED bitmap, and the shared sequence counter wraps cleanly. Pinned in Task 8.
- A mismatch at any output site (call site, cap word, either buffer slot) fails the whole install with nothing written; unload reverts all of them; hook off installs exactly like version 7. Pinned in Task 5.
- With the packet buffer moved, pad input and every other Bluetooth HID send and receive keep working. Pinned by the Task 6 console checks (input still translated, DS3 untouched).

---

### Task 1: Facts note (read-only) - DONE (commit bd6d333)

The gate failed (buffer of 72 bytes) and led to spec Amendment 1; the note holds every value Tasks 5-8 need. Nothing further to do here.

**Files (kept for the record):**
- Create: `docs/superpowers/notes/2026-11-03-ds3-route-output-facts.md`

**Interfaces:**
- Produces: the settled facts later tasks use: whether the shared packet buffer `0x4326C8` has room for 79 bytes, any length limit below `0xF8D60` (the L2CAP sender `0x116EF0` and its callees), every caller of `0x10283C` / `0xF8D60` / `0x2A2C30`, the DS3 payload `0xCC054` builds (length, rumble and LED offsets), the FNV hashes of the qwords holding `0x2A2DE8` and `0xF8D74`, and the stub contract for a `bl` entry.

- [ ] **Step 1:** Disassemble with the scratchpad capstone helpers over `tools/dumps/dex492/ds45pad_lv2.bin` (run from outside the repo with `-I`; `dis.py` in the workspace root shadows the stdlib). Answer each fact above; list every load of the TOC slot at `-0x3370` to find who else uses `0x4326C8` and what follows it.
- [ ] **Step 2:** Compute the two FNV hashes with the repo's rule (offset basis `0xcbf29ce484222325`, prime `0x100000001b3`, bytes in memory order over the 8-aligned qword).
- [ ] **Step 3:** Write the note. **Gate:** if the buffer lacks room for 80 bytes or a lower layer caps the length below 77, stop and report to the user; the spec says the design is then reopened.
- [ ] **Step 4:** Commit the note.

### Task 2: BRANCHL patch kind

**Files:**
- Modify: `src/kpatch.h`, `src/kpatch.c`
- Test: `tests/test_kpatch.c`

**Interfaces:**
- Produces: `DS45_KPATCH_BRANCHL` (after `DS45_KPATCH_BRANCH`; `value` is the absolute target, encoded as `bl target`, opcode 18 with LK = 1) and `int kpatch_branchl_word(uint64_t from, uint64_t to, uint32_t *word)`, same contract and range as `kpatch_branch_word`. `DS45_KPATCH_MAX` raised from 8 to 12 (the output patches bring one transaction to 10 entries); the journal arrays follow it.

- [ ] **Step 1:** Add tests: `test_branchl_encoding` (forward, backward, ±range limits, misaligned target, NULL word), `test_branchl_verify_apply_revert` (hash checked, out-of-range refused, apply then revert restores the qword in both halves), `test_branchl_hash_mismatch_writes_nothing`, `test_patch_list_limit` (12 patches apply and revert; 13 are refused with -1 and nothing read).
- [ ] **Step 2:** Run `tests/run_tests.ps1`; expect `test_kpatch` to fail to build or fail.
- [ ] **Step 3:** Implement the kind and `kpatch_branchl_word` next to the BRANCH code, sharing the range check.
- [ ] **Step 4:** Run the host tests; expect all pass.
- [ ] **Step 5:** Commit.

### Task 3: Data block v8, config keys, trace

**Files:**
- Modify: `shim/shim_abi.h`, `src/ds45pad.h`, `src/config.c`, `src/kshim.c` (`build_data_block`), `src/main.c` (shimdbg line)
- Test: `tests/test_config.c`, `tests/test_ds3report.c` (abi layout), `tests/test_kshim.c` (data block fields)

**Interfaces:**
- Produces: `DS45_SHIM_VERSION 8`; `DS45_SHIM_PID_SITES 7` (sites 0-2 as today, 3 = output call site, 4 = cap word, 5 = buffer TOC slot, 6 = buffer-plus-one TOC slot) and `pid_orig[7]`; appended to `ds45_shim_data_t`: `uint64_t orig_output` (code address of `0x10283C`, written from the firmware row), `uint32_t out_mode, out_seq, dbg_out_calls, dbg_out_ds, dbg_out_translated, dbg_out_passed, dbg_out_last_len, dbg_out_cap_len`, `uint8_t dbg_out_cap[64]`, `uint8_t out_buf[80]` (the 78-byte DualSense report), `uint8_t pktbuf[96]` (the moved packet buffer, 8-aligned, zero), padded to a multiple of 8. Config fields `int shim_output_hook` (default 1) and `int shim_output_mode` (default 0, clamped 0..2) with VSH-only keys `SHIM_OUTPUT_HOOK` and `SHIM_OUTPUT_MODE`, boot marks `cfg_shim_output_hook` / `cfg_shim_output_mode`. The shimdbg line gains ` outc= outds= outxl= outps= outlen=` and a separate `outcap` line with the captured payload as hex, printed when it changes.

- [ ] **Step 1:** Write the failing tests: key parsing like `test_shim_stable_id_key` (default, 0, junk, last wins, mode clamp), the new `test_abi_layout` offsets and size, and `test_kshim` checks that `out_mode` and `orig_output` land in the block.
- [ ] **Step 2:** Run the host tests; expect failures in those three suites.
- [ ] **Step 3:** Implement the struct, version bump (update the `== 7` checks in `test_kshim.c` and `test_ds3report.c`), config parsing, `build_data_block` writes and the trace additions. Regenerate the blob.
- [ ] **Step 4:** Run the host tests; expect all pass.
- [ ] **Step 5:** Commit.

### Task 4: Hook decision, entry stub, packer export (observe only)

**Files:**
- Modify: `shim/shim.h`, `shim/shim.c`, `shim/entry.c`, `tools/shim_pack.py`, `tools/test_shim_pack.py`
- Test: `tests/test_shim.c`

**Interfaces:**
- Consumes: Task 3 fields; `shim_pad_is_dualsense`.
- Produces: `int shim_output_filter(ds45_shim_data_t *d, const uint8_t *buf, uint16_t len, uint8_t *rid, const uint8_t **out_buf, uint16_t *out_len)` returning 1 when the call must be rewritten (not in this task: it always returns 0). It always bumps `dbg_out_calls`, records `dbg_out_last_len` and copies `min(len, 64)` payload bytes to `dbg_out_cap` (any pad), and bumps `dbg_out_ds` for a DualSense (`pad = buf - 0x22B`) and `dbg_out_passed` when it returns 0. NULL `d`, NULL `buf` or `len` 0 or over 64: counters only, return 0. The blob exports `shim_output`, a code entry (offset constant `DS45_SHIM_OFF_OUTPUT`) reached by `bl` from the kernel: save LR, r2 and r3-r7, adopt the blob TOC PC-relatively from a rebased literal, call the C body, then restore r2, LR and r1, replace r5, r6, r7 only if the body returned 1, and `bctr` to `orig_output` so the original returns straight to the kernel.

- [ ] **Step 1:** Tests in `test_shim.c`: `test_output_counts_and_captures`, `test_output_ds3_pad_untouched` (a pad with the Sony class but PID `0x0268`: return 0, arguments unchanged), `test_output_bad_arguments` (NULL buf, len 0, len 65, `enabled` 0). In `test_shim_pack.py`: the `shim_output` export and its `DS45_SHIM_OFF_OUTPUT` constant appear in the generated header, and the rebased literal is handled like the descriptor words.
- [ ] **Step 2:** Run the host and packer tests; expect failures.
- [ ] **Step 3:** Implement the C functions, the stub in the `__asm__` block of `entry.c` (follow the existing framed wrappers) and the packer export/rebase of the literal. The host cannot run the stub; its first live run is Task 6, which is why observe mode comes first.
- [ ] **Step 4:** Run host and packer tests; build the blob.
- [ ] **Step 5:** Commit.

### Task 5: Installer rows and sites

**Files:**
- Modify: `src/kshim.h`, `src/kshim_table.c`, `src/kshim.c`
- Test: `tests/test_kshim.c`

**Interfaces:**
- Consumes: `DS45_KPATCH_BRANCHL`, `DS45_KPATCH_MAX` 12, Task 3 fields (`pktbuf`, `orig_output`, `output_patches[4]`), the facts note's hashes.
- Produces: row fields `output_patches[4]` and `orig_output`; DEX 4.92 values: site 3 BRANCHL at `0x80000000002A2DE8` (target `base + DS45_SHIM_OFF_OUTPUT`, expect `0x2A29E6BB7BA10D5E`); site 4 WORD at `0x80000000000F8D74` = `cmplwi cr7,r31,0x4D` (`0x2B9F004D`, expect `0xBF27C253F5228E13`); site 5 PTR at `0x8000000000372650` (expect the current value `0x80000000004326C8`, new value the address of `pktbuf` in the blob); site 6 PTR at `0x8000000000372660` (expect `0x80000000004326C9`, new value `pktbuf + 1`); `orig_output = 0x800000000010283C`. Sites 3, 5 and 6 apply when `shim_output_hook` is set (any mode); site 4 only when it is set and `shim_output_mode` is not 0. `pid_site`, `pid_capture`, `pid_patched_qword` (add the PTR arm: the whole qword is replaced), the range check and adoption handle the new sites and kinds; `KSHIM_FINAL_MAX` (10) and the adoption journal check stay within `DS45_KPATCH_MAX` (12). Apply order inside the transaction: buffer slot 5, then slot 6 (back to back), then the call site, then the cap word.

- [ ] **Step 1:** Tests: `test_output_sites_gated` (hook off: none; mode 0: call site and both buffer slots; mode 1 and 2: those plus the cap word), `test_output_branchl_follows_load_base`, `test_buffer_slots_point_into_blob` (both slots hold `pktbuf` and `pktbuf + 1` at the load base; old values restored on remove), `test_output_site_mismatch_fails_clean` (each of the four sites in turn; snapshot equality, zero pokes), `test_output_remove_restores_all`, `test_output_adopt_v8` (adopt restores every applied site; a v7 block is not adopted), extend `test_builtin_table` and the pristine-image row test.
- [ ] **Step 2:** Run host tests; expect failures.
- [ ] **Step 3:** Implement the row, table and installer changes.
- [ ] **Step 4:** Run host tests; expect all pass.
- [ ] **Step 5:** Commit.

### Task 6: Console stage 1, observe

**Files:**
- Modify: `docs/superpowers/notes/2026-11-03-ds3-route-output-facts.md` (add the capture)

- [ ] **Step 1:** Build the debug plugin; deploy with `tools/ps3_console_deploy.ps1 -Ip 192.168.1.97 -Dest tools/dumps/dex492/output1 -Config` the usual keys plus `SHIM_OUTPUT_HOOK = 1`, `SHIM_OUTPUT_MODE = 0`; confirm the sites read back as patched (call site a `bl` into the blob, both buffer slots pointing at `pktbuf` and `pktbuf + 1`, cap word unchanged).
- [ ] **Step 2:** Arm the DualSenses by Bluetooth address with `tools/ps3_console_linkkeys.ps1 -Arm -Bd`; ask the user to power on the DS3, then the DualSense(s), and to start the XMB controller vibration test and change controller numbers (reconnect).
- [ ] **Step 3:** Read `ps3_console_trace.ps1`. Pass: no freeze, `outc` rising, DS3 and DualSense behave as before, **and pad input still works through the moved buffer** (`xl` rising for the DualSense, the DS3 usable in the XMB). Record the DS3 payload with the motors on and off (the 48 bytes, the duration bytes) in the facts note, and in particular **whether the kernel sends a zero-motor report when a rumble ends** or relies on the duration expiring. That answer decides the rumble translation: if it relies on expiry, stop and report to the user before Task 7.
- [ ] **Step 4:** Restore with `tools/ps3_console_restore.ps1`. Commit the note. **Gate:** a freeze or a pad regression stops the plan for the user.

### Task 7: Pure translator

**Files:**
- Create: `shim/ds3_output.h`, `shim/ds3_output.c`
- Test: `tests/test_ds3output.c` (`/* SOURCES: ../shim/ds3_output.c */`)

**Interfaces:**
- Produces: `#define DS_OUT_LEN 78`; `int ds3_output_to_dualsense(const uint8_t *in, int in_len, uint8_t seq, uint8_t out[DS_OUT_LEN])` (`in` is the 48-byte DS3 payload without the report id; 0 and all 78 bytes written, or -1 with `out` untouched for NULL, `in_len` not 48, or a payload not of its shape); `void ds3_output_probe(uint8_t seq, uint8_t out[DS_OUT_LEN])` (fixed player-1 colour and LED, no rumble). Offsets and the LED/colour table are named constants taken from the facts note: player LEDs `0x04, 0x0A, 0x15, 0x1B` for ports 1-4, colours blue, red, green, pink; lowest set LED bit chooses the port; none set means LEDs and light bar off; large motor copied, small motor 255 or 0; valid flags set on every report.

- [ ] **Step 1:** Write the tests: `test_header_and_flags`, `test_rumble_mapping`, `test_led_and_colour_per_port` (all four ports, several bits set, none set), `test_crc_matches_reference` (an independent reference loop in the test file, prefix `0xA2`), `test_sequence_nibble_wraps`, `test_declines_bad_input` (NULL, empty, short, wrong shape; `out` untouched), `test_no_read_past_len` (guard bytes, every length), `test_fuzz`, `test_probe_report`.
- [ ] **Step 2:** Run; expect the suite to fail to build.
- [ ] **Step 3:** Implement with a bitwise CRC (no table) and no static data.
- [ ] **Step 4:** Run; expect pass.
- [ ] **Step 5:** Commit.

### Task 8: Probe and full modes in the hook

**Files:**
- Modify: `shim/shim.c`
- Test: `tests/test_shim.c`

**Interfaces:**
- Consumes: Task 7 functions; `out_buf` holds the whole 78-byte report.
- Produces: `shim_output_filter` rewrites for a DualSense pad when `enabled` and mode is 1 (probe report) or 2 (translation of the payload): `*rid = 0x31`, `*out_buf = d->out_buf + 1`, `*out_len = 77`, `out_seq` advanced mod 16, `dbg_out_translated` bumped, return 1. A translation that returns -1 passes through. Mode 0 never rewrites.

- [ ] **Step 1:** Tests: `test_output_probe_rewrites`, `test_output_full_rewrites_exact_bytes`, `test_output_two_pads_use_own_leds` (two pad objects with different LED bits, one shared counter), `test_output_malformed_dualsense_passes`, `test_output_ds3_never_rewritten_all_modes`, `test_output_mode0_never_rewrites`.
- [ ] **Step 2:** Run; expect failures.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run host tests; build the blob.
- [ ] **Step 5:** Commit.

### Task 9: Console stage 2, probe

**Files:**
- Modify: the facts note (record the result)

- [ ] **Step 1:** Deploy with `SHIM_OUTPUT_MODE = 1`; arm one DualSense; power it on while the DS3 is already connected.
- [ ] **Step 2:** Read the trace and ask the user what the DualSense light bar shows. Pass: the probe colour appears, `outxl` rises, no disconnect or freeze, the DS3 is unaffected. Record whether the pad accepted output in basic input mode.
- [ ] **Step 3:** Restore. Commit the note. **Gate:** the pad ignoring the report, dropping, or a freeze stops the plan; the spec says full-report input then comes first.

### Task 10: Console stage 3, full, default and docs

**Files:**
- Modify: `src/config.c` (default `shim_output_mode` 2), `tests/test_config.c`, `README.md`, the spec's status line

- [ ] **Step 1:** Deploy with `SHIM_OUTPUT_MODE = 2`; with the DS3 and two DualSenses, ask the user to run the XMB vibration test, check each pad's LEDs and colour against its controller number, force-power-off and reconnect one pad.
- [ ] **Step 2:** Pass: all of the spec's success criteria. Restore.
- [ ] **Step 3:** Change the default to 2 with its test, document the keys in the README, set the spec status to implemented, run all host tests and the four builds.
- [ ] **Step 4:** Push, open the PR with the verification list and anything not verified, and bind it.

---

## Amendment (spec Amendment 2): receive-side support for the full input report

The probe run showed the DualSense's input stops once it is sent an output report (facts note F8). Tasks 9 and 10 wait for the tasks below. The facts task comes first and its result decides the patch rows; the later tasks are written once it is done.

### Task 11: Receive path facts (read-only)

**Files:**
- Modify: `docs/superpowers/notes/2026-11-03-ds3-route-output-facts.md` (add section F9)

**Interfaces:**
- Produces: the settled facts for the receive-side patches: (1) the whole path from the transport input read (slot `0x37CE38`, the hook's original `0x33A010`) down to the HID receive function for the interrupt-in channel (`0xF9060`) and how a received packet reaches the caller's buffer `dst` of length `len_in`: is the copy bounded by `len_in`, does it truncate, what does the read return; (2) what the four `li r7,0x41` read limits (sites `0xF8484`, `0xF9094`, `0xF9134`, `0xF91F4`) do, what `0x1169E4` does with a packet longer than the limit and the capacity of the interrupt-in receive ring for the DualSense's connection; (3) the typical `len_in` and `dst` size the pad driver passes (caller of the input slot, around `0x2A4E00`); (4) a recommended patch set with the exact new instruction words, hash rows (FNV of the aligned qwords) and the effect on the DS3's 50-byte reports and on other users of the same functions; (5) what the input hook (`shim/entry.c`, the `rc > 0x41` guard and `len_in` check) and `shim_input_filter` must change to accept a 79-byte report.

- [ ] **Step 1:** Disassemble and trace read-only with the scratchpad capstone helpers over `tools/dumps/dex492/ds45pad_lv2.bin` (as Task 1).
- [ ] **Step 2:** Write section F9. **Gate:** if the copy into `dst` is not bounded by `len_in` (an overflow with a 79-byte packet), or the ring cannot hold a 79-byte packet, state it prominently and stop for the user.
- [ ] **Step 3:** Commit the note.
