# DS45Pad: Any-Firmware Bluetooth Support - Design

**Date:** 2026-10-28
**Status:** Approved in conversation, awaiting written-spec review
**Sub-project:** 2 of 4 in the "transparent once loaded" effort
(1 Silent operation, 2 Any firmware, 3 No Port 7 step, 4 Auto in-game DS3 identity).
Sub-project 2 runs first because the Bluetooth path cannot run on the available test hardware.

## 1. Intent

### What the user said
- Using the plugin must be completely transparent once it is loaded.
- Bluetooth must work on any firmware, not only CEX 4.93.
- No degraded mode: kernels are verified beforehand, with a fallback match performed when the plugin loads.
- If nothing matches, the plugin steps aside and shows one toast.
- Test hardware: a DEX console on CFW (Evilnat) 4.92 using external Cobra stage2 from USB, set up as a debugging station; a DECR reference tool on DEX 4.75. Neither will be upgraded. ProDG and the Cell SDK are available.

### Assumptions
- Syscall 6 (`sys_cfw_peek`) is available on every supported kernel (CFW, HEN, DEX with Cobra).
- The LV2 text/data region from `0x8000000000000000` up to a fixed bound is mapped on every 4.7x-4.9x kernel. The bound is set from the collected dumps and webMAN MOD's LV2 dump range, and confirmed during planning.
- The Bluetooth HID handler on each kernel reaches the raw report through a pointer held in a TOC slot, as on CEX 4.93 (`TOC 0x34FBB0`, slot `TOC-0x3680`, report at `+0xE8`).

### Success criteria
- On DEX 4.92 and DEX 4.75 the physical PS button works over Bluetooth, with the offsets found by a table match.
- With the table entry removed, the load-time scan finds the same offsets, writes the cache, and the next boot uses the cache.
- On a kernel nothing matches, the console never panics, no Bluetooth virtual pad is created, USB is unaffected, and exactly one toast explains it.
- No peek is ever issued at or above the LV2 bound.

### Out of scope
Port 7 removal and menu behaviour (sub-project 3); the logging, notification and cleanup work (sub-project 1); automatic game-module loading (sub-project 4).

## 2. Current state

`passthru.c` peeks `0x800000000034FBB0 - 0x3680` once (`s_bt_init_done`) and, if the result merely *looks* like an LV2 address, reads the report at `+0xE8` and accepts any quadword starting with `0xA1`. On any kernel other than CEX 4.93 this reads an unrelated kernel structure and can inject phantom input. The origin of the three constants is not documented anywhere in the repository.

## 3. Design

### 3.1 Kernel offsets record

```c
typedef struct {
    const char *name;          /* "DEX 4.92", for trace and toast only     */
    uint64_t    fp_addr;       /* LV2 text address of the fingerprint      */
    uint64_t    fp_hash;       /* 64-bit hash of the 32 bytes at fp_addr    */
    uint64_t    slot_addr;     /* absolute address of the bt_work pointer  */
    uint32_t    report_off;    /* raw report offset inside bt_work          */
} ds45_kernel_t;
```

The fingerprint is the instruction pair that loads the `bt_work` slot (the CEX 4.93 equivalent of `ld rX, -0x3680(r2)`). If it matches, the slot and report offsets are right by construction, whatever the version string, HFW/OFW variant or Cobra patch level says.

Lives in a new `kernel.c` / `kernel.h` (VSH build only). `passthru.c` asks it for a resolved `ds45_kernel_t` and no longer contains any address.

### 3.2 Matching chain (runs once at startup, before the poll loop)

1. **Table.** For each built-in entry, peek the 32 bytes at `fp_addr` (always below the bound), hash them, and compare with `fp_hash`. The first match wins.
2. **Cache.** Read `/dev_hdd0/plugins/ds45pad/kernel_cache.txt`. Accept its record only if its stored fingerprint matches when peeked again.
3. **Scan.** Scan LV2 text `[base, bound)` for the instruction pattern common to the verified kernels (derived with `lv2scan.py`, §3.5). From a hit, derive the TOC value (from the kernel's function descriptors), the slot address (`TOC + displacement`) and the report offset. On success, write the record to the cache. The debug trace records how long the scan took.
4. **No match.** Do not register a Bluetooth virtual pad. The console's generic driver runs the controller untouched; USB support is unaffected. The single boot toast becomes `DS45Pad: Bluetooth not supported on this kernel`.

PS3MAPI `GET_FW_VERSION` / `GET_FW_TYPE` and the step that matched are written to the trace. They never influence which offsets are chosen.

Until a CEX 4.93 dump is available, the CEX 4.93 entry has no fingerprint. It keeps today's address-range check and relies on the cross-check (§3.3).

### 3.3 Live cross-check (gates every source: table, cache, scan)

Calling `cellPadGetData` from the plugin in the VSH consumes pending data and steals XMB input, so the check is passive:

- **Ground truth.** The existing VSH detours on `cellPadGetData` / `cellPadGetDataExtra` additionally record the last sample the VSH itself read for the Bluetooth source port.
- **Comparison.** Decoded raw reports must agree with those samples (sticks within ±2, digital buttons equal) over at least 50 matching changes.
- **Structural checks, always on.** Header bytes `A1 01`; hat value ≤ 8; `bt_work` pointer inside the LV2 heap range; report stops changing within 200 ms of the source port disconnecting.
- **Before passing.** No virtual pad is registered. In the XMB the native driver carries input; in a game with a Port 7 setup, input arrives about 1-2 s after the controller connects.
- **On failure.** The offsets are not used, the cache entry is deleted, and the plugin steps aside as in §3.2 step 4.
- **Hardware unknown.** It is not yet confirmed that the VSH reads the source port through the detoured descriptor (the review found contradicting assumptions in `main.c` and `passthru.c`). This is the first on-console verification. If the detour never sees source-port reads, the fallback is the structural checks plus "report changes only while the port reports connected". The plan must come back to the user before relying on that weaker check.

### 3.4 Debug LV2 dumper (`DS45_DEBUG` builds only)

- Enabled by `DUMP_LV2 = 1` in `ds45pad.txt`; the key is ignored in release builds.
- Runs once after `ANNOUNCE_MS`. It copies `[base, bound)` with syscall 6 peeks to `/dev_hdd0/tmp/ds45pad_lv2.bin`, writes `/dev_hdd0/tmp/ds45pad_lv2.txt` (PS3MAPI version/type, bound, plugin version, matching result), and shows a `LV2 dump written` toast.
- Skips if the dump already exists, so leaving the flag on cannot fill the disk.

This is the debug-build variant introduced in sub-project 1 (`build.ps1 -Debug`, `make DEBUG=1`, outputs `*_debug.sprx`). If sub-project 1 has not landed when this is implemented, this sub-project adds the minimal `DS45_DEBUG` build switch itself.

### 3.5 Offline analysis tool: `tools/lv2scan.py`

Python with Capstone (PPC64, big-endian). Given one or more dumps it:

1. Recovers the kernel TOC from the function descriptors.
2. Finds candidates for the Bluetooth HID handler: code comparing against `0x054C` / `0x0268` and `0xA1`, loading a pointer from `r2±off`, and accessing the report offset.
3. Prints each candidate's fingerprint, slot address and report offset as a ready-to-paste `ds45_kernel_t` entry.
4. Given several dumps, prints the instruction pattern common to all of them, which becomes the scan pattern of §3.2.

Workflow per new kernel: run the debug build with `DUMP_LV2=1`, copy the files off over FTP, run `lv2scan.py`, review the candidate, add the table entry, then confirm with the debug-build cross-check trace on that console before shipping it in a release.

Dumps contain Sony kernel code and are **never committed**. They live in a git-ignored `tools/dumps/`.

## 4. Error handling summary

| Situation | Behaviour |
|---|---|
| Fingerprint peek returns a syscall error | Treated as no match; continue the chain |
| Cache file missing or malformed | Skip to scan |
| Cached fingerprint mismatch | Ignore the cache, scan |
| Scan finds 0 or more than 1 distinct candidate | No match (ambiguity is never guessed) |
| Cross-check fails | Delete the cache entry, step aside, toast |
| `bt_work` slot is NULL at first poll | Retry each rescan tick (not once only, unlike today) |

## 5. Testing

- **Host C tests** (existing clang→wasm harness, extended): matching chain against a fake `peek` over a synthetic LV2 image (table hit, cache hit with fingerprint re-check, stale cache rejected, scan hit written to cache, scan miss leading to step-aside and the toast text, and an assertion that no peek is at or above the bound); the cross-check state machine (pass only after 50 matching changes, mismatch deletes the cache entry, no LDD registration before a pass, each structural check fails safe); the dumper skipping when a dump exists.
- **`lv2scan.py` tests (pytest):** synthetic PPC64 images with hand-encoded instruction words for TOC recovery and candidate finding. Real dumps run as local-only fixtures when present in `tools/dumps/`.
- **Build checks:** vsh/game × release/debug all build with `-Wall`. A `strings`/symbol check confirms the dumper and the debug trace tags are absent from release builds. The game build gains no new imports.
- **On-console, in order:**
  1. Debug trace confirms the VSH detour sees source-port reads (resolves §3.3's unknown).
  2. DEX 4.92 and DEX 4.75 dumps are produced.
  3. With table entries added: the trace shows a table match and a cross-check pass, and the PS button works over Bluetooth.
  4. A build with the table entry removed forces the scan: it finds the same offsets and writes the cache; the second boot uses the cache.
  5. A deliberately broken scan pattern gives step-aside and the toast, with no panic.

## 6. Proprietary material policy

The repository must never contain Sony-proprietary material.

- **Never committed:** Cell SDK headers, libraries, stub archives and documentation; scetool keys and its `data/` directory; PUPs, decrypted `lv2_kernel` images and LV2 dumps; `kernel_cache.txt`; ProDG project and capture files; any binary linked against the SDK (`*.sprx`, `*.prx`, `*.sym`, `*.self`, `*.elf`, and the generated `vshmain` import stub `*.a`, which is produced at build time). Release binaries are build output, not repository content.
- **Allowed:** handwritten interoperability declarations (struct layouts, NIDs, constants), as `patches/compat_pad.h`, `tests/stubs/` and `ds45pad.h` already do, and addresses and offsets, which are facts about a kernel.
- **Fingerprints are hashes.** `ds45_kernel_t` stores a 64-bit hash of a 32-byte LV2 code window (`fp_addr`, `fp_hash`) rather than raw instruction bytes. No kernel code enters the repository, and the fingerprint is stronger. §3.1 and §3.2 step 1 use this hash.
- **Scan patterns are opcode masks.** They are expressed as instruction-encoding masks (for example "`ld` from `r2`, any displacement, any target register"), never copied byte sequences.
- **`lv2scan.py` output stays local.** It prints disassembly for review on the user's machine; only the derived entries (addresses, offsets, hashes) are committed.
- **Enforcement:** an expanded `.gitignore`, and `tools/check_no_proprietary.ps1`, installed as a pre-commit hook and also runnable by hand. It rejects banned extensions and paths, binary files outside an explicit allowlist, and SDK copyright markers in text files.
- **Repository history:** the new work goes in a **new repository with fresh history**, created in a sibling directory of the upstream clone (which stays untouched as a read-only reference). Its first commit imports the current source tree minus every banned file, so no binary exists anywhere in its history. This spec moves into that repository. Nothing is pushed to any remote without the user's go-ahead.
- **Credit to the original authors:**
  - The GPLv2 `LICENSE` and every existing copyright/licence header in the source are preserved unchanged.
  - A `CREDITS.md` names the original project and author, [domyno6773/DualSense-PS3](https://github.com/domyno6773/DualSense-PS3) (imported at commit `33ba29e`, v2.0.0, by way of the user's fork [Desulation/DualSense-PS3](https://github.com/Desulation/DualSense-PS3)). It states that this is an independent continuation for two reasons: the scope of the changes makes upstreaming impractical, and the upstream history contains SDK-linked, retail-signed `.sprx` binaries that carry Sony IP risk, which a fresh history avoids. It carries over the existing credits: Dobridp (original DS45Pad), OsirisX (PS3xPAD), aldostools (webMAN MOD; source of the vshmain NIDs) and the PS3Xploit Team (PS3HEN).
  - The upstream README's donation link belongs to domyno6773 and stays in `CREDITS.md` under their name. It is not presented as the new project's own.
  - The README keeps its credits section and links `CREDITS.md`.
  - The README gains a short **"Why there are no binaries in this repository"** note. Signed `.sprx` files contain SDK stub code and are only loadable because of leaked retail keys, so they are build output and never repository content. Whether to publish signed builds elsewhere is a separate, deliberate decision. It also points to `tools/check_no_proprietary.ps1` for contributors.
  - The first commit message states the upstream repository and commit it was imported from.

## 7. Open items resolved during planning
- The exact LV2 bound (from the dumps and webMAN MOD's LV2 dump range).
- The exact scan pattern (from `lv2scan.py` across the DEX 4.92 and DEX 4.75 dumps).
- The cache file format (single line, versioned).
- The "LV2 heap range" used by the structural check in §3.3 (from the `bt_work` pointer values seen in the dumps).
