# Asking someone to build it

Building requires the Cell SDK, which is proprietary. Anyone who already has the
environment set up can do it in seconds — and there are plenty of people in
that situation on the PS3 forums.

Below is a ready-to-post text.

**Where to post:** [PSX-Place](https://www.psx-place.com) (PS3
Homebrew/Development section) or [GBAtemp](https://gbatemp.net) (PS3 hacking).

Before posting, put the project somewhere it can be downloaded — a GitHub repo
is ideal, because then the person can clone it and build directly.

---

## Text to copy

> **[Request] Build help: NativePadPS3 — DualShock 4 / DualSense VSH plugin**
>
> I wrote a PS3 VSH plugin that gives DS4 and DualSense controllers proper
> DualShock 3 behaviour — working PS button, PS3 conformity so games that
> demand an official pad accept them, and a DS3-matched analog stick response
> curve with automatic calibration (which also fixes Hall effect sticks that
> never reach the ends of their range).
>
> I don't have the Cell SDK, so I can't build it. Would anyone with the
> toolchain be willing to compile it? The code is self-contained — 13 files, no
> external dependencies beyond the SDK stubs.
>
> **Source:** `<link to your repository>`
>
> **Build:**
> ```
> cd src && make
> ```
>
> **Requirements:**
> - Cell SDK PPU toolchain (`ppu-lv2-gcc`, `-mprx`), `CELL_SDK` set
> - Stubs: `usbd_stub`, `io_stub`, `fs_stub`, `sysmodule_stub`
> - `scetool` with retail keys
>
> **Important — signing:** the Makefile already uses key revision `0A` (3.55),
> which PS3HEN requires. Please don't change it to `04`; that's what forced
> PS3xPAD to be re-released as "3.55 signed for HEN".
>
> ```
> scetool -0 SELF -1 TRUE -s FALSE -2 0A -3 1070000052000001 \
>         -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000 \
>         -e nativepadps3.prx nativepadps3_vsh.sprx
> ```
>
> `make` alone produces the VSH build, which is the only one needed. There's
> also `make game` for an optional in-game rumble module, but rumble is off by
> default so it can be skipped.
>
> The code has never been compiled, so warnings or build errors are entirely
> possible — if anything fails to build, please paste the output and I'll fix
> it. Thanks!

---

## If they ask for changes

If the person reports a build error, bring me the exact output of `make`.
First-build errors are expected — the code has never been through a compiler,
and the most likely trouble spots (SDK constant names that vary between
versions, the syscall macro header) are mapped in the common-errors table in
[BUILD.md](BUILD.md#common-errors).
