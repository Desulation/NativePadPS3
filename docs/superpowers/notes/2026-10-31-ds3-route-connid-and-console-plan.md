# DS3-route conn-id resolution and the R2 console plan (DEX 4.92)

Scope: settle how the DS45Pad shim obtains the per-connection id it must pass as
`0x8000000000116EF0`'s first argument when it sends the DualSense feature-`0x05`
GET, and write the on-console steps that confirm the approach. Read-only static
analysis only: no console was touched and no product code was changed. Values are
settled from `analysis/ff_492.json` + `analysis/lv2_code_492.bin` (LV2 offset =
byte offset) and the dumps in `tools/dumps/dex492/` (file offset = LV2 offset;
`ds3route/*.bin` carry live driver/connection/pad objects), with the repo venv
driving `fn.py`/`dis.py`. Builds on `2026-10-30-ds3-route-facts.md` (F1 start-up,
F2 drain) and `2026-10-31-ds3-route-get-primitive.md` (the GET recipe; "GET note"
below). Conventions as in those notes: kernel addresses `0x80000000xxxxxxxx`, a
bare `0xNNNN` is an LV2 offset, code sites carry an FNV-1a-64 of the 8-aligned
qword that contains them. No raw kernel bytes or pasted disassembly listings
appear here — mnemonics-in-prose, opcode masks, addresses, observed field values
and FNV hashes only.

Headline results:
- **The conn_id is a 48-bit Bluetooth device address, packed exactly one way, and
  matched in FULL 64 bits** by the re-resolver — the prior reviewer is right and
  GET note §1's "`conn_id & 0xFFFF`" is wrong for the path `0x116EF0` uses (§2).
- **Start-up CAN self-derive it safely.** The self-derive sequence is the stock
  Bluetooth SET-sender's own lock discipline with a different header byte, so its
  lock-safety is settled, modulo the pre-existing F1 table→Bluetooth binding (§1).
- **Verdict: option (a), self-derive** — it eliminates the conn_id cache/capture
  entirely and makes the DualSense path self-contained like a DS3 (§3). Option (c)
  (read the live id from a fixed address) is the minimal console-test vehicle (§4).

---

## 1. THE KEY QUESTION — can start-up self-derive the conn_id? (settled: yes, safe)

**The handle in hand.** The start-up hook `0x80000000000CA194` receives the driver
object in r3 and carries only the 1-byte connection handle at `drv+0xAC` (F1). The
state-7 handler `0x0CA2CC` reads exactly that byte (`lbz r3,0xAC(r31)` at
`0x0CA318`) and passes it to the Bluetooth SET sender `0x0102750` (`bl` at
`0x0CA334`). So `drv+0xAC` is precisely the value the senders feed the lookup.

**`0x80000000001010B8` — prototype and behaviour (settled).**
```
int conn_lookup(u8 handle /*r3*/, void *out /*r4*/);
```
It zeroes a local, calls the table walk `0x80000000000F719C(r3=&scratch, r4=handle)`,
and on a hit writes the **connection object pointer** to the caller's `*out`
(a pre-zeroed qword local) and returns 0; on a miss it sets r31 to `0x80010005`
(`lis r31,-0x7FFF; ori r31,r31,5`), calls the cleanup `0x0F70A8`, and returns that
error. Its only callees are `0xF719C` and `0xF70A8` — **no lock helper** — so it
takes no lock of its own and requires its caller to hold the outer lock. Both
existing callers honour that: `0x0101E20` (F2 drain) and `0x0102750` (SET) each
load the lock from TOC `-0x3078` and `bl 0x13C1C` *before* calling `0x1010B8`,
releasing with `bl 0x13C14` after. The caller passes the handle masked to a byte
(`clrldi r3,r29,0x38` at both `0x0101E6C` and `0x01027B4`).

**The two locks (both verified from the dump).**
- **Outer** `0x80000000004325D0`: TOC `-0x3078` holds `0x80000000004325D0`, and
  `*0x4325D0` is the live lock id `0x80000000051A280` (observed in every dex492
  dump). Acquired by the generic helper `0x13C1C` (which sets r5=0 and tail-branches
  to `0x1F60C`) with the lock id in r3 and r4=0; released by `0x13C14` (tail-branch
  to `0x1ED48`). It guards the connection-table walk (`0x1010B8`/`0xF719C`) and the
  shared scratch buffer `0x80000000004326C8`.
- **Inner** `0x8000000000434778`: TOC `-0x28C0` holds `0x8000000000434778`. It is
  taken by **`0x116EF0` itself**: at `0x116F2C` it `bl 0x112EC8`, which does
  `ld r3,-0x28C0(r2)` and blocks with timeout -1 (set by the `li r3,-1` at
  `0x116F00`) through `0x11AA04`; it is released on every exit via the `b 0x112E90`
  tail at `0x116F74`, which reloads `-0x28C0(r2)` and calls `0x11AB74`. (This
  **corrects the `ff_492.json` call graph**, which omits both the
  `0x116EF0`→`0x112EC8` edge and the `0x116F78`→`0x1125F4` edge; the disassembly
  shows them plainly, and the GET note's own fingerprint for the acquire site —
  the `0x116F28` qword, FNV `0x195540DB4E14E236` — re-validates here.)

**Lock order is outer-then-inner, and that is the stock send's order.** `0x0102750`
acquires the outer lock (`bl 0x13C1C` at `0x01027B0`) and *still holds it* when its
body `0x0F8BD8` reaches `bl 0x116EF0` at `0x0F8D04`, where `0x116EF0` takes the inner
lock; `0x0102750` then releases outer at `0x0102800`. There is one global order:
inner is only ever taken with outer held (or with no lock held, e.g. the F2 drain's
inner take also sits inside its outer-locked region).

**Start-up is lock-free on entry.** `0xCA194` does `stdu`, saves regs, checks
`arg1` (`cmpdi r4,0`), reads the substate byte (`lbz r9,0x54(r3)`; bounded
`cmplwi …,8`) and dispatches through its jump table — it never loads a lock from
the TOC and never calls `0x13C1C`. Its state-7 handler `0x0CA2CC` then calls
`0x0102750`, which **acquires the outer lock from scratch**. Because the stock DS3
enable works on this firmware, the outer lock cannot already be held when `0xCA194`
is entered — `0x0102750`'s own acquire of the same lock would otherwise deadlock.
So a hook installed in `0xCA194`'s place likewise runs with the outer lock free and
can acquire it.

**Hazard analysis (make-or-break).** A self-derive that mirrors `0x0102750` —
acquire outer, `0x1010B8` lookup, pack, `0x116EF0` send (inner taken under outer,
the stock order), release outer — has:
1. **no double-acquire** of the outer lock (it is free on entry, proven by the DS3
   path);
2. **no order inversion** (outer-then-inner, identical to every other caller;
   `0x116EF0` takes only the inner lock and nothing it calls re-takes the outer, or
   the stock SET would already deadlock);
3. **no corruption against the concurrent F2 drain**, which serialises on the same
   outer lock and, for the channel FIFOs, on the inner lock inside `0x116EF0`.

It is the kernel's own proven Bluetooth-send lock discipline with the GET header
byte substituted. **Self-derive lock-safety is therefore settled.** The single
residual is not a lock question at all: whether a DualSense actually dispatches
through `0xCA194` (slot 7) in the same thread/lock context as a DS3. That is F1's
already-flagged, *inferred* table→Bluetooth binding (no data pointer to those
tables exists in the dump), it affects the whole shim equally, and §4's passive
"no freeze at the GET send" check settles it on-console.

**Installer fingerprints for the targets self-derive adds** (FNV of the 8-aligned
qword; hasher re-validated against three GET-note/fact-sheet hashes):
- `0x1010B8` — interior qword `@0x1010E0` (its `bl 0xF719C`): `0x03A6404540AF95C6`.
  (Its entry qword is the `stdu r1,-0xA0(r1); mflr r0` prologue it shares byte-for-
  byte with `0x116EF0`, so it is *not* unique — use this interior one.)
- `0x13C1C` outer-acquire helper — qword `@0x13C18`: `0x7C6054EE0A66C934`.
- `0x13C14` outer-release helper — qword `@0x13C10`: `0xDF2902056BD0386E`.

---

## 2. The exact conn_id composition (and the full-64-bit correction)

**The re-resolver and the compare width (correction, settled).** `0x116EF0`'s
worker `0x116F78` calls `0x80000000001125F4` with `li r3,5; mr r4,conn_id`
(`0x116F7C`/`0x116F78`). In `0x1125F4` a `clrlwi r0,r3,0x1D` keeps the low 3 bits
of r3 and a `cmpwi …,5` routes the r3==5 case to `0x1126B4`. There an
`ld r0,0x10(r9)` then **`cmpd cr7,r0,r4`** at `0x1126BC` compares the **full 64-bit**
field at *connection-entry+0x10* against conn_id; the table loop repeats the same
full `cmpd` at `0x1126E0` over entries of stride `0x14E8`. The create path even
stores the whole id with `std r4,0x10(r31)` at `0x112754`.

The 16-bit masking the GET note described is real but belongs to **other** r3 modes:
the r3==4 path (`0x11262C` region) does `clrlwi …,0x10` and a `cmpw` against a
half-word at entry+0, and the r3==6 path (`0x11272C` region) does the same against a
half-word at entry+`0x1418`. `0x116EF0` uses **neither**. So for our path the conn_id
must equal entry+0x10 in all 64 bits — i.e. the exact packed value, not just its low
16 bits. **This is a correction to GET note §1.** Fingerprints: the r3==5 compare
qword `@0x1126B8` = `0xA59FE9BEEEF98B55`; the loop compare qword `@0x1126E0` =
`0x4B62F71CF22A45CE`; the worker's lookup call qword `@0x116F78` = `0x14B3EF2B82EAAECE`.

**The packing (byte-exact, identical at both send sites).** Let
`sub = *(connobj + 8)` — a pointer the code loads with `ld r9,8(connobj)`
(`0x0F91E4` in the F2 drain `0x0F91C0`; `0x0F8C98` in the SET body `0x0F8BD8`). Both
sites then stage a 32-bit word from `sub+8` (`lwz`), a byte from `sub+0xC` (`lbz`)
and a half-word from `sub+0xE` (`lhz`), and assemble r3 with a fixed ladder of
`sldi`/`slwi`/`or` (shift amounts `0x28`, `0x20`, `0x18`, `0x10`, `8`) before
`bl 0x116EF0`. Writing `sub[k]` for the byte at `sub+k`, the result is:

```
conn_id = (sub[0xE] << 40) | (sub[0xF] << 32)      // half-word at sub+0xE
        | (sub[0xC] << 24)                          // byte at sub+0xC
        | (sub[0x9] << 16) | (sub[0xA] << 8) | sub[0xB]   // low 3 bytes of word at sub+8
```

i.e. a 48-bit Bluetooth device address zero-extended to 64 bits; the word's byte 0
(`sub[0x8]`) and the byte at `sub+0xD` are not used. The two call sites are
byte-identical (fingerprints: F2 pack region `@0x0F91E0` = `0xAC2E27C49FE0310D`; SET
pack start `@0x0F8C98` = `0xCA996A07FE90B82B`), and the SET site fixes the direction
with `li r4,2; li r5,2` at `0x0F8CAC`/`0x0F8CB0` — control-out, report transfer.
Because the kernel itself packs exactly this and the result round-trips through the
full `cmpd`, replicating this packing is guaranteed to re-resolve.

**Live confirmation.** The active connection table resolves from TOC `-0x2848` to
base `0x80000000004B0170` (stride `0x14E8`; TOC `-0x2838` = `0x4B1658` is the loop
base one entry in). The conn_id field of the live slot, `0x80000000004B0180`
(= base+0x10), reads **`0x0000A0AB51A6C913`** in both `ds3route/type01_connected.bin`
and `ds3route/navpid_connected.bin` (same connected controller) and
**`0x00004CB99B1324A8`** in `ds45pad_lv2.bin` — all 48-bit values with the top 16
bits zero, exactly the packing's shape. The six array entries beyond the active slot
read zero in these snapshots (one controller connected).

---

## 3. VERDICT and recipe — option (a), self-derive

**Recommendation: (a) self-derive in start-up.** It is the only option that removes
the conn_id cache/capture dependency and makes the DualSense path autonomous like a
DS3 (the project's native-DS3 standard: no plugin babysitting of a per-connection
id, no prompts). Its lock-safety is settled (§1): the sequence is the stock
`0x0102750` discipline with the GET header, not a novel pattern. It is also the only
option that is correct for multiple controllers without extra machinery, because
`0x1010B8` maps each handle to its own connection object directly. Options (b) and
(c) are covered below as fallbacks.

**Concrete change (keeps shim.c pure; the kernel work stays in entry.c glue).**
The pure `shim_startup_dualsense` already takes the id from the data block and sends
correctly; self-derive only changes *where the id comes from*. Add a freestanding
helper in `shim/entry.c`, e.g. `entry_derive_conn_id(const void *drv, uint64_t *out)`,
that reproduces `0x0102750`'s prologue through data-block descriptors:
1. `lockid = *(volatile uint64_t *)outer_lock_addr;`  // outer_lock_addr = 0x80000000004325D0
2. acquire: call the `0x13C1C` descriptor with `(lockid, 0)`.
3. `handle = *((const uint8_t *)drv + 0xAC);` zero a local qword `co`; call the
   `0x1010B8` descriptor with `(handle, &co)`.
4. if `co == 0` → release and fail; else `sub = *(const uint64_t *)(co + 8)` and
   pack `*out` exactly per §2.
5. send: build `d->pdu = {0x43, 0x05}` and call `d->send_routine`
   `(conn_id, SHIM_SEND_DIR_CTRL_OUT, SHIM_SEND_OPCLASS_REPORT, d->pdu, SHIM_GET_PDU_LEN)`
   while the outer lock is still held (inner taken under outer — the stock order).
6. release: call the `0x13C14` descriptor with `(lockid)`.

`shim_startup` (entry.c) then, for an enabled DualSense, derives the id this way and
hands it to the existing body instead of reading `d->conn_id[slot]`. The per-slot
send budget (`start_tries`, `SHIM_START_TRIES_MAX`) is unchanged and still bounds
retries.

**ABI additions (a version-3 append, mirroring how `send_opd`/`send_routine` were
added in v2).** The data block gains, all filled by the installer from the
fingerprinted row and never touched by the plugin at run time:
- `lookup_opd[3]` + `lookup_routine` — an ELFv1 `{entry=0x80000000001010B8, toc, 0}`
  and its in-blob address (same build-an-OPD-in-.data pattern as `send_opd`).
- `lockacq_opd[3]` + `lockacq_routine` — for `0x8000000000013C1C`.
- `lockrel_opd[3]` + `lockrel_routine` — for `0x8000000000013C14`.
- `outer_lock_addr` — the constant `0x80000000004325D0`.

With self-derive, `conn_id[]` and `kshim_set_connection`'s id argument become
unnecessary. **What the plugin still owes:** the DualSense-identity flag. The hook
args carry no VID/PID (F1/U1), so the plugin must still mark which handles are
enabled DualSenses (`conn_state[]`/`conn_key[]`) — a lighter `kshim_set_connection`
that records only the handle. Self-deriving VID/PID too (reading it from `connobj`
or a linked pad object) is not established statically and stays out of scope.

**Freestanding?** Yes. Self-derive adds only indirect calls through three more
data-block descriptors, one absolute-address load, small immediates, a qword local
and the 2-byte PDU buffer. No SDK, libc, `.rodata` or new relocations; all callees
are pre-existing kernel code, and the F5 i-cache flush already covers the blob.

**Fallbacks.**
- **(b) plugin-side capture via `kshim_set_connection(handle, conn_id)`.** Workable
  but heavier and it moves the hard part (handle→connobj→pack) to the plugin, where
  it is harder: the plugin has no easy handle→connobj path (that is `0x1010B8`,
  which it would have to drive via the spare-syscall trick under the outer lock, or
  replicate). For a *single* connected controller it can shortcut by reading the
  live id straight from `0x80000000004B0180` (§2) at connect, but that does not
  generalise to several controllers (the table entry carries no handle to select by).
- **(c) hand-set for the console test only.** Read the live id from the fixed
  address `0x80000000004B0180` with a syscall-6 passive read, confirm it is a 48-bit
  value, and poke it into `conn_id[slot]` (and re-arm `start_tries[slot]`). This
  needs no lock, exercises only the already-settled `0x116EF0` send, and is the
  cleanest way to confirm R2 **before** the self-derive glue is trusted (§4).

---

## 4. The R2 console confirmation plan (passive, RAM-only)

Goal: confirm R2 — that a feature-`0x05` GET flips *this* DualSense on *this*
firmware to report `0x31` — while riding along the F1 binding, U3 executability and
(for self-derive) the lock-liveness checks. For a session at the DEX with the user.
Everything observed is a syscall-6 passive read; nothing prompts or confirms, per
the native-DS3 standard.

**Setup**
1. **Deploy the debug shim; it self-installs on boot.** Confirm the boot trace shows
   `kshim_installed` and record the blob base and data-block base `B = g_base +
   DS45_SHIM_OFF_DATA` (all later reads are fields relative to `B`). A `kshim_already`
   trace is equally fine (adopted). No trace, or `kshim_verify_FAILED` → install /
   fingerprint problem, not R2: stop here.
2. **Flip DualSense #1 pairing type `0x08`→`0x01`** with the debug tool, verified and
   read-back (as done before), so the pad enumerates on the Sony-pad Bluetooth path.
3. **Connect the pad.**
4. **Make the conn_id available per the recommended option.** For this first bring-up
   use **(c)** to isolate R2 from the new glue: passively read `0x80000000004B0180`,
   confirm a 48-bit value `V`, and poke `conn_id[slot]`=`V` with `start_tries[slot]`=0
   via the (identity-only) plugin path. (Keep the self-derive build for the follow-up
   run in step 7.)

**Watch (syscall-6 reads only)**
- Data-block counters at `B`: `stat_startups`, `stat_translated`, `stat_passed`, and
  `start_tries[slot]`; plus `conn_state[]`/`conn_key[]`/`conn_id[]`.
- The shared scratch buffer `0x80000000004326C8` (the CAPTURE_LV2 window): snapshot
  its first `0x41` bytes right after input activity and look for a report whose id is
  `0x31` (the drain strips the leading `0xA1`, so the id lands at buffer byte 0 as the
  F2 path hands it on).
- System liveness through the connect (no watchdog/freeze) and whether a controller
  **port** is assigned (the pad reaches "running").

**PASS (full chain).** After connect, with no freeze: `stat_startups` goes `0`→`≥1`
(the GET send returned `≥0`), then within a few polls `stat_translated` begins to
climb and/or a `0x31`-id report appears in `0x4326C8`, then a port binds.
This single observation confirms, at once:
- **R2 = yes** — feature-`0x05` flips this pad/firmware to `0x31`.
- **conn_id / send path** — the packed 48-bit id matched entry+0x10 in full and
  `0x116EF0` accepted it (validates §2 end-to-end).
- **F1 table→Bluetooth binding = confirmed** — our `0xCA194` hook ran for the
  DualSense (`stat_startups` moved) and the F2 hook ran (`stat_translated` moved).
- **U3 executability = confirmed** — blob code executed at all (the pad enumerated
  via `shim_pid_ok` and the counters moved), so pool-`0x22` memory is executable.

**FAIL taxonomy (what each means / next step).**
- Pad never enumerates → `shim_pid_ok` did not run or the product-ID path differs
  (U3 / F4), not R2; the test is blocked until enumeration works.
- `stat_startups` stays 0, no freeze → either the hook never fired for the DualSense
  (F1 binding wrong) or, on the self-derive build, `0x1010B8` returned `0x80010005`
  so no id was built. With the (c) pre-stage in place, if it *still* stays 0 the
  `0xCA194` hook is not firing for the DualSense → **F1 binding refuted**.
- `stat_startups` moves but **no `0x31`** ever appears and `stat_translated` stays 0
  → **R2 = no** on the send alone, or the reply must be drained first → run the
  control-in (`dir=3`) drain experiment (GET note R1/§2).
- `0x31` appears but no port binds → the F2 translation gate
  (`buf[0]==0x01 && buf[1]!=0xFF`) / running handshake, not R2.
- **Freeze/watchdog exactly at the GET send** (self-derive build only) → the hook's
  outer-lock acquire deadlocked, i.e. the DualSense's `0xCA194` invocation *did* hold
  the outer lock on entry, against the DS3 inference → **self-derive lock-safety
  refuted**; fall back to (c)/(b), where the hook takes no outer lock (with a
  pre-staged id it only calls `0x116EF0`, which takes the inner lock alone).

**Follow-up run (promote to self-derive).** Once R2 is confirmed under (c), switch to
the self-derive build, clear `conn_id[slot]`, reconnect, and confirm: no freeze,
`stat_startups` moves again, and the same `0x31`→port result reproduces. That single
run confirms the lock dance is live-safe and the in-kernel `0x1010B8`+pack reproduces
the id read at `0x4B0180`.

---

## 5. Cannot-settle-statically list (with the on-console check for each)

- **Lock-context for the DualSense (the §1 residual, = F1 binding).** Whether a
  DualSense dispatches through `0xCA194` in the same lock-free context as a DS3 — the
  only thing between "self-derive is the stock discipline" and "self-derive is safe
  *here*." Not provable statically (no pointer to the slot-7 tables exists; dispatch
  is indirect). Check: §4's "no freeze at the GET send" plus `stat_startups` moving.
- **R1 — control-in (`dir=3`) FIFO population.** Unchanged from GET note §2/§5:
  whether inbound feature replies are enqueued for a `dir=3` drain. Not needed for
  the enable; check only if `0x31` never appears.
- **R2 — empirical flip to `0x31`.** The whole point of §4; a console write/observe.
- **R4 — reply/stream timing** (how many polls after the send `0x31` begins).
- **VID/PID from the hook args (U1, unchanged).** Still not derivable from `drv`;
  the plugin supplies the DualSense-identity flag. Self-derive settles the conn_id,
  not the identity.

Per the project gate, this non-empty list means the controller should involve the
user for the §4 console session before any step that writes a console.

---

## Self-review

- Every §1–§2 claim carries a function, an instruction offset with its mnemonic/mask,
  an observed field value, or an FNV hash; the hasher is re-validated against the
  known `0x29B8`/`0x116F28`/`0x116F40` hashes. The `ff_492.json` call graph is shown
  to be lossy (two missing edges) and superseded by disassembly where they conflict.
- §1 resolves the make-or-break: `0x1010B8` holds no lock and needs the outer; the
  outer is free on `0xCA194` entry (proven by the stock DS3 path acquiring it inside
  the same subtree); the self-derive order is the stock `0x0102750` outer-then-inner
  discipline — so lock-safety is settled, with the one genuine residual isolated to
  F1's inferred binding and given a passive console check.
- §2 corrects the GET note: the r3==5 re-resolver uses a full-64-bit `cmpd` against
  entry+0x10, not a 16-bit mask; the packing is pinned byte-for-byte from two
  independent, hardware-proven call sites and cross-checked against three live
  48-bit conn_id values in the dumps.
- §3 recommends (a) with a freestanding, shim.c-pure change and the exact ABI
  additions, and states honestly what self-derive does *not* remove (the identity
  flag / U1). §4 gives an all-passive plan whose single PASS confirms R2, R3, the F1
  binding and U3 together, with a FAIL taxonomy that names the one freeze signature
  that would refute self-derive and the (c)/(b) fallback for it. §5 lists the
  read-only residuals with their checks.
