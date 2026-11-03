# DS3-route GET primitive — feature-report GET recipe for the shim (DEX 4.92)

Scope: turn fact-sheet F3's partial finding into a concrete, callable recipe for a
Bluetooth HID **GET_REPORT of feature 0x05** on CFW DEX 4.92, so the DS45Pad shim can
switch a DualSense to its full report `0x31`. Read-only static analysis only; no console
was touched and no code was changed. All values are settled from
`analysis/ff_492.json` + `analysis/lv2_code_492.bin` (LV2 offset = byte offset) and the
8 MiB dump `tools/dumps/dex492/ds45pad_lv2.bin` (LV2 offset = file offset), with the repo
venv driving `fn.py`/`dis.py`. Builds on `2026-10-30-ds3-route-facts.md` (F1 start-up
context, F2 input drain, F3 send side). Conventions as in the fact sheet: kernel addresses
`0x80000000xxxxxxxx`, a bare `0xNNNN` is an LV2 offset, code sites carry an FNV-1a-64 hash
of the 8-aligned qword that contains them. No raw kernel bytes or pasted disassembly
appear here — opcode/field descriptions, mnemonics-in-prose and masks only.

---

## 1. `0x116EF0` full prototype and both-direction argument convention

`0x8000000000116EF0` is **not** the transfer engine itself; it is a small (136-byte)
self-locking dispatcher. It saves the five incoming registers, then:

```
int h116ef0(u64 conn_id /*r3*/, int dir /*r4*/, int opclass /*r5*/,
            void *buf /*r6*/, int len /*r7*/);
```

- It moves `r3→r31` (conn_id), `r4→r28` (dir), `r5→r29` (opclass), `r6→r27` (buf),
  `r7→r26` (len), loads `r3=-1` and calls the **lock-acquire** stub `0x80000000000112EC8`
  (which loads the lock object from TOC slot `-0x28C0` and acquires it with timeout `r4=-1`
  = block). TOC `-0x28C0` resolves to lock object **`0x8000000000434778`** (file 0x373100).
- If acquire returns non-zero it falls to the epilogue, which tail-calls
  `0x80000000000112E90` (release that lock via `0x800000000011AB74` and return the code).
- Otherwise it validates `dir(r28) ≤ 3` (`cmplwi …,3`; opcode `ble`); `dir > 3` returns
  LV2 error `0x8001000000000002`. For `dir ≤ 3` it branches to the worker at
  `0x8000000000116F78` (the finder does not mark this as its own function; it is the
  continuation of `0x116EF0`).
- The worker calls `0x80000000001125F4(r3=5, r4=conn_id)` to resolve the connection object
  (`0x1125F4` matches `conn_id & 0xFFFF` against a connection table; low 3 bits of its `r3`
  select the table, here 5), checks a status bit (mask `0x00004000`) in `*connobj`, then
  **dispatches on `opclass(r5)` through a jump table** whose pointer is at TOC slot `-0x25F0`
  = `0x8000000000116FE4` (entries are 32-bit self-relative offsets). `opclass` is bounded
  `≤ 0xC`; `table[2]` = `0x80000000001170F0` is the **HID report-transfer** handler.
- `table[2]` (`0x1170F0`) checks `*connobj` bit mask `0x04000000`, then calls the core
  **`0x80000000001169E4(r3=connobj, r4=dir, r5=buf, r6=len, r7=0)`** and returns its result.

Return value: `>= 0` on success (byte count moved), negative `0x8001xxxx` LV2 error
otherwise (e.g. `…0002` bad dir/len, `…002D` wrong channel mode, `…000D`/`…0028`
transfer error). `0x116EF0` propagates the core's result via `r30`.

### The direction argument and what each value means

The core `0x1169E4` uses `dir(r4)` **twice**: (a) as an index into a per-direction L2CAP
channel-descriptor array — `chan = connobj + dir*0x48 + 0x12C0` (arithmetic at the qword
`@0x116A68`: `extsw r25,r4; sldi …,6; sldi …,3; add; addi …,0x12C0`); and (b) as the
read/write selector — `dir ∈ {0,2}` take the **write/enqueue** path, `dir ∈ {1,3}` take the
**read/drain** path. With exactly four legal `dir` values (`≤ 3`) this is two bidirectional
L2CAP channels, each with an even TX slot and an odd RX slot:

| dir | channel slot | semantics | observed use |
|----:|--------------|-----------|--------------|
| 0   | connobj+0x12C0 | write (TX) | — (interrupt-out; unused) |
| 1   | connobj+0x1308 | read (RX)  | **input-report drain** (F2) |
| 2   | connobj+0x1350 | write (TX) | **SET_REPORT send** (F1 enable) |
| 3   | connobj+0x1398 | read (RX)  | — (**control-in; unused by the DS3 driver**) |

So `dir` is both the channel and the direction; `opclass(r5)=2` is the operation class
(HID report transfer). This refines F3's "`r4`: 1=read, 2=write": `r4` selects a *channel
slot whose parity is the direction*, and `r5=2` (not `r4`) is the report-transfer class.

### Cross-check against the two existing call sites

- **Input drain (F2), wrapper `0x80000000000F91C0`** (reached from `0x101E20`): it builds
  `conn_id` by packing `*(connobj+8)` (word), `+0xC` (byte), `+0xE` (half) and calls
  `0x116EF0(conn_id, dir=1, opclass=2, buf=0x80000000004326C8, len=0x41)` **twice** in a
  loop (immediates `li r4,1; li r5,2; li r7,0x41` at `0x0F91E8/0x0F91EC/0x0F91F4`), then
  memsets the shared buffer. `dir=1` = interrupt-channel RX read. This is the F2 drain.
- **SET send (F3), body `0x80000000000F8BD8`** (sole caller `0x102750`): it memsets the
  shared buffer `0x80000000004326C8`, writes header byte `reportType | 0x50` to `buf[0]`
  (`ori r0,r31,0x50` at `0x0F8C7C`), report id to `buf[1]`, memcpys the payload at `buf+2`
  via `0x8000000000082A08`, packs `conn_id` from `*(r27+8)` the same way, and calls
  `0x116EF0(conn_id, dir=2, opclass=2, buf=0x4326C8, len=payloadlen+2)` (`li r4,2; li r5,2`
  at `0x0F8CAC/0x0F8CB0`). `dir=2` = control-channel TX write. This is the SET sender.

Both confirm `opclass=2`, buffer+len conventions, and that `conn_id` is the BD-address/handle
composite re-resolved by `0x1125F4`. Lock object `0x434778` and jump-table pointer
`0x116FE4` are read straight from the dump.

**Fingerprints (installer may verify before calling):**
`0x116EF0` dir-check qword `@0x116F40`: FNV `0xF1A285B923A50ABE`; its lock-acquire-call
qword `@0x116F28`: FNV `0x195540DB4E14E236`. Report-transfer dispatch `0x1170F0`
transfer-call qword `@0x117108`: FNV `0x380811A43609BD8C`. Core `0x1169E4` channel-arith
qword `@0x116A68`: FNV `0xBBB0BD7C59FC99C4`. (The entry qwords of `0x116EF0` and `0x1010B8`
are byte-identical prologues — `stdu r1,-0xA0(r1); mflr r0` — so an entry-qword hash is
**not** unique; use the interior qwords above.)

---

## 2. The feature-report GET transaction, and how/where the reply is delivered

### The request PDU and length

A GET differs from the SET body only in the first PDU byte. SET writes `reportType | 0x50`
(`0x51` output, `0x53` feature). A GET_REPORT of a **feature** report uses header
**`0x43`** (`GET_REPORT`=0x40 | `feature`=0x03) followed by the report id `0x05`; there is
no payload. The shim therefore builds a 2-byte PDU `{0x43, 0x05}` and sends it exactly as
SET does but with `dir=2, opclass=2`, i.e. `0x116EF0(conn_id, 2, 2, &pdu, 2)`. (`0x1169E4`'s
write path prepends a 2-byte L2CAP length itself, so `len` is the PDU length only — 2 here,
mirroring SET's `payloadlen+2`.) F3 already established there is no `ori …,0x43` anywhere in
the HID region and `0x0F8BD8`/`0x102750` hard-code `0x50`, so the shim cannot reuse the SET
sender and must assemble the `0x43` PDU itself.

### What the send physically does (settled)

Core `0x1169E4`, write path (`dir=2`): it validates free space in the control-channel TX
descriptor, appends the 2-byte length then the PDU via `0x800000000011E640`, and — because
`dir==2` (branch at `@0x116B84`) — reaches the control-channel flush at `0x116D04`, which
(after a `0x00400000`-masked state check on `*(connobj+0x12B8)`) calls
`0x8000000000113828(connobj, 2)` to transmit. This is the same enqueue-and-flush the DS3
enable uses on real hardware, so the `0x43/0x05` request will go out on the HID control
channel.

### How the reply is delivered — the make-or-break fact

**The transport is asynchronous and queue-based; there is NO synchronous request→reply
coupling.** `0x116EF0`/`0x1169E4` with a read `dir` do not perform a control transaction and
wait for an answer — the read path simply **drains whatever is already sitting in that
channel's RX FIFO** into the caller's buffer and returns the count (0 if nothing/too little
is queued). Evidence, core `0x1169E4` read path: it reads the queued-byte count at
`chan+0x10`, peeks a 2-byte length via `0x800000000011E440(chan,&n,2,0)`, and if enough bytes
are present copies them into the **caller's buffer `r26`** via the same `0x11E440` at
`0x116D40..0x116D50` (`mr r4,r26 /*dst*/; … bl 0x11E440`), returning the count; if too few are
queued it consumes and returns 0 (`0x116B1C`/`0x116D2C` paths). The alternate channel mode
(`*connobj` bit `0x40000000`, at `0x116BB0`) does the same into `r26`. The read path even
inspects the HID transaction header of received control frames (it special-cases a `0xA2`
DATA|output echo at `0x116C88`), confirming control-channel inbound DATA flows through this
same drain.

Consequently a feature-report GET on this stack is a **two-phase, non-blocking** exchange:

1. **Send** the `{0x43,0x05}` request on the control channel — `0x116EF0(conn_id, 2, 2,
   &pdu, 2)`.
2. The DualSense's reply (a control-channel DATA frame, header `0xA3` = DATA|feature,
   report id `0x05`, calibration payload) arrives **asynchronously** on the **control-channel
   RX FIFO**. To read it the shim would drain the control-in slot — `0x116EF0(conn_id,
   dir=3, 2, &rbuf, rcap)` — in a later call/tick, exactly analogous to how F2 drains the
   interrupt-in slot with `dir=1`. The drained bytes include the leading `0xA3` header (the
   read path does not strip it; F2's leading-`0xA1` strip is done later by its own caller,
   not by `0x116EF0`).

**What is settled statically:** the delivery *mechanism* (asynchronous; via a per-channel RX
FIFO drained by a dir-indexed read into the caller's buffer; never a synchronous return from
the send) and the exact drain call shape (control-in is `dir=3, opclass=2`, by the
even-TX/odd-RX channel-pairing proof in §1).

**What CANNOT be settled statically (make-or-break residual):** whether this firmware's
Bluetooth stack actually **enqueues** inbound control-channel feature replies into the
`dir=3` RX FIFO for the HID layer to drain. The DS3 driver **never issues a `dir=3,
opclass=2` read anywhere in the dump** (confirmed: no call site uses `dir=3` with `opclass=2`),
so that receive path is unexercised and its wiring cannot be observed — the frames might be
queued there, or consumed/dropped by a lower handshake layer first. This is the drain side
F3/U2 flagged, now pinned to a specific slot (`dir=3`) and mechanism (async FIFO), with the
one unknown isolated. Note the enable does **not depend** on the shim reading the reply: the
purpose of the GET is to make the pad start streaming `0x31`, and the running transition is
produced by F2 translating those interrupt-in (`dir=1`) reports — so the `dir=3` drain is
best-effort (clear the control channel / confirm the transaction) and the recipe is robust
even if `dir=3` never yields.

---

## 3. Callability from the F1 start-up context, and the lock contract

**Two distinct locks are involved**, and conflating them was the main risk:

- **Outer lock `0x80000000004325D0`** (TOC `-0x3078`, loaded in `0x102750` at `0x010275C`):
  taken by the *transport-manager* level — the SET sender `0x102750` (acquire via
  `0x8000000000013C1C` at `0x01027B0`, release via `0x8000000000013C14` at `0x0102800`) and
  the input reader `0x101E20` (F2). It protects the **shared scratch buffer
  `0x80000000004326C8`**, the transport `+0x167` "sent" flag, and the connection-table walk
  done by the lookup `0x80000000001010B8` (which takes **no lock of its own** — verified: its
  body calls `0x800000000000F719C` with no `0x13C1C`, so its caller must hold `0x4325D0`).
- **Inner lock `0x8000000000434778`** (TOC `-0x28C0`): taken by **`0x116EF0` itself** (acquire
  `0x112EC8`, release `0x112E90→0x11AB74`). It protects the L2CAP channel FIFOs and the flush.

Lock order in the normal send is outer-then-inner (`0x102750` takes `0x4325D0`, then
`0x116EF0` takes `0x434778`). F1/F3 established the **start-up hook context holds neither
lock** (`ca194`'s state-7 handler `0x0CA2CC` acquires `0x4325D0` only transiently inside its
own `0x102750` call; the dispatch entry is lock-free).

**Therefore `0x116EF0` is callable from the start-up wrapper**, and the cleanest design takes
**only** the inner lock, which `0x116EF0` takes for itself:

- If the shim calls `0x116EF0` **directly** with (a) a **connection id it already holds** (so
  it does not need the `0x1010B8` lookup) and (b) its **own PDU buffer** (so it never touches
  the `0x4325D0`-protected shared buffer `0x4326C8`), then the only shared state it mutates is
  the channel FIFOs, which `0x116EF0` guards with `0x434778`. **No outer lock is needed, and
  there is no double-lock hazard** — the shim never also calls `0x102750`, and `0x434778` is
  not held on entry to the start-up hook. This *satisfies* F3's "do not pre-acquire
  `0x4325D0`" guidance by not needing `0x4325D0` at all (F3's warning assumed routing through
  the self-locking `0x102750`, which the `0x43`-header requirement rules out).
- A concurrent F2 input drain (holding `0x4325D0`, draining `dir=1`) and the shim's send
  (`dir=2`) serialise correctly on `0x434778` inside `0x116EF0`, so they cannot corrupt the
  channel state.

Fallback only if the shim must re-derive the connection id in-blob (see §4 Design B): it
would call `0x1010B8` and therefore **must** hold `0x4325D0` across lookup+send (acquire
`0x13C1C`, release `0x13C14`), reproducing `0x102750`'s outer-then-inner order. That is still
deadlock-free from the lock-free start-up context, but it is heavier and reintroduces the
outer lock; Design A avoids it.

---

## 4. The concrete shim change

### 4a. Start-up wrapper (`shim_startup_dualsense` / its `d->get_report` glue)

The pure `shim_startup_dualsense` is already correct in shape: it calls
`get(drv, SHIM_HID_FEATURE=0x03, SHIM_DS_CALIB_REPORT_ID=0x05)` and, on success, returns
`SHIM_F1_OK` so the driver keeps polling; F2 then drives 3→running by translating the `0x31`
reports. Only the **`get_report` primitive** (today a null descriptor) needs to be a real
blob routine. Recommended (**Design A**), the glue does:

1. Resolve the connection id for this `drv`. `drv+0xAC` is only the 1-byte handle; the value
   `0x116EF0` needs is the BD-address/handle **composite** that `0x1125F4` matches (its low 16
   bits are the key). The plugin already fills a per-connection slot at connect time (entry.c
   `conn_key[]`), when it has the pad/connection objects in hand — it should record that
   composite there, so the blob reads it with no runtime lookup and no outer lock (ties the
   capture to residual U1; see §5).
2. Build the 2-byte PDU `{0x43, 0x05}` in a **blob-owned buffer** (a field in
   `ds45_shim_data_t`, never `0x4326C8`).
3. Send: `h116ef0(conn_id, /*dir*/2, /*opclass*/2, &pdu, /*len*/2)` through the `0x116EF0`
   descriptor. On `>= 0`, bump `stat_startups` and return `SHIM_F1_OK`; on negative, return it.
4. Reply: do **not** block for it. Optionally, on a later start-up tick, drain the control-in
   slot `h116ef0(conn_id, /*dir*/3, 2, &rbuf, rcap)` best-effort to clear the channel; the
   running transition comes from the F2 `0x31`→DS3 translation regardless (§2).

This keeps `shim_get_report_fn(ctx, type, id)` intact: `ctx` = the cached connection id,
`type=0x03`→header `0x43`, `id=0x05`, mapped to `dir=2, opclass=2` inside the glue.

### 4b. Installer table / ABI fields (replacing `get_report = 0`)

- In `ds45_shim_kernel_t` (and the kshim_table row), replace the single `get_report` zero with
  the **`0x116EF0` transport descriptor**: `{ entry = 0x8000000000116EF0, toc =
  0x80000000003759C0 }` (same OPD-less, place-an-OPD-in-the-data-block pattern F5 uses for the
  i-cache flush). Carry the **convention constants** alongside it (they are firmware-stable but
  belong with the row for clarity): control-out `dir=2`, control-in `dir=3`, report-transfer
  `opclass=2`, GET feature header `0x43`, feature report id `0x05`, `connobj = *(obj+8)` and the
  BD-address field offsets `+0x8/+0xC/+0xE` (needed only by Design B's in-blob packing).
- The **data block** (`ds45_shim_data_t`) carries: the `0x116EF0` function descriptor the blob
  calls through; the blob-owned PDU/reply buffer; and the per-connection composite connection
  id captured by the plugin (extend `conn_key[]`, or add a parallel `conn_id[]`, to hold the
  full 64-bit composite rather than just the handle byte).
- Verify-before-call fingerprints for the new call target are in §1; the row's existing
  firmware fingerprint (`0x80000000000F3380` / `0xDF192C9D78181BF7`) already pins the row to
  this kernel.

### 4c. Freestanding?

**Yes.** The GET adds no SDK, libc, or new `.rodata`/relocation needs: it is one (optionally
two) indirect call(s) through a function descriptor held in `.data`, plus a 2-byte buffer and
small immediates. It calls only pre-existing kernel code, so it writes no new executable bytes
beyond the blob itself (which the F5 i-cache flush already covers). Design A needs just the
`0x116EF0` descriptor; Design B would additionally need descriptors for `0x1010B8`, the lock
helpers `0x13C1C`/`0x13C14`, and the lock-object pointer `0x4325D0` — all still freestanding
data-block fields, but avoidable.

---

## 5. Residual uncertainty (cannot be settled read-only) and the cheapest on-console check

Cannot-settle-statically list:

- **R1 (make-or-break, = refined U2 drain side).** Whether this firmware's BT stack enqueues
  inbound control-channel feature replies into the `dir=3` RX FIFO. The `dir=3, opclass=2`
  read path exists and would copy such bytes into the caller's buffer, but **no code in the
  dump ever exercises it**, so whether the reply actually lands there (vs being consumed by a
  lower handshake layer) is unobservable statically. The delivery *mechanism* is settled
  (asynchronous FIFO drain, control-in = `dir=3`); only the *population* of that FIFO is open.
- **R2 (= U2 empirical).** Whether a feature-`0x05` GET actually flips *this* DualSense on
  *this* firmware to report `0x31`, and whether the request alone triggers it or the host must
  also read the reply to complete the transaction.
- **R3 (= U1).** The exact composite-connection-id form / `drv`→connection linkage for building
  `0x116EF0`'s `r3` at runtime. Design A sidesteps it in the blob by having the plugin capture
  the composite at connect, but the capture itself is the U1 residual (the plugin has the
  objects at connect, so it is tractable plugin-side, not blob-side).
- **R4.** Real reply timing (how many ticks after the send a `0x31`/reply appears).

**Single cheapest on-console confirmation:** have the shim issue only the **send**
(`0x116EF0(conn_id, 2, 2, {0x43,0x05}, 2)`) at start-up and then **passively watch the
interrupt-in path** (the F2 drain / the shared buffer `0x4326C8`, or the `stat_translated`
counter) for a report whose id is `0x31`. If `0x31` reports begin arriving after the send,
that single observation confirms the whole recipe at once: the conn-id/packing and send path
work (R3), and feature-`0x05` flips the pad (R2) — and it shows the reply drain is unnecessary
(R1 moot). It needs nothing beyond the send the shim already performs plus the plugin's
existing passive (syscall-6) read, matching the native-DS3 standard (a start-up control-channel
transaction like the DS3 enable, verified passively, no prompts). Only if `0x31` does **not**
appear is the control-in (`dir=3`) drain experiment (R1) needed.

---

## Self-review

- §1 prototype, the dual role of `dir` (channel index `connobj + dir*0x48 + 0x12C0` **and**
  even-TX/odd-RX selector), `opclass=2` jump-table entry `0x1170F0`→core `0x1169E4`, and both
  lock objects are read directly from the dump and cross-checked against the F2 drain
  (`0x0F91C0`, `dir=1`) and SET (`0x0F8BD8`, `dir=2`) call sites; every claim carries an
  address, an instruction offset, an observed immediate, or an FNV hash (verified: the hasher
  reproduces the fact sheet's `0x29B8` → `0x56636AA045D470E1`).
- §2 settles the reply-delivery *mechanism* (asynchronous RX-FIFO drain into the caller's
  buffer, control-in = `dir=3`, non-blocking) and states plainly the one thing that is **not**
  statically determinable — whether that FIFO is populated for control-channel feature replies
  — because the DS3 driver never reads it. Per the task gate, that make-or-break fact is called
  out as console-only, with the recipe made not to depend on it.
- §3 separates the inner lock `0x434778` (self-taken by `0x116EF0`, safe to call) from the
  outer `0x4325D0` (needed only for the `0x1010B8` lookup), and shows Design A needs neither a
  pre-acquire nor a replicate, removing the F3 double-lock hazard.
- §4 is implementable without re-derivation; §5 lists R1–R4 with the single cheapest passive
  on-console check that confirms the whole chain.
