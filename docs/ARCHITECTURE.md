# Architecture

## The central decision

The project started with the wrong architecture and took many iterations to
arrive at the right one. It is worth recording both, because understanding why
the first could not work is what justifies the second.

### What doesn't work: a virtual pad (LDD)

The natural idea is to create a conforming virtual controller and wait for the
game to talk to it. It is what PS3xPAD does, and what this project did for a
long time.

**It cannot work for games that require an official controller.** An LDD pad
always reports `device_type = CELL_PAD_DEV_TYPE_LDD` (5). Never `STANDARD` (0).
A game that checks the type rejects every virtual pad, from every plugin,
always. Measured on the console: asking for capability `0x1F`, the system grants
`0x07` (conformity, pressure and sixaxis) and the type stays `5`, unchangeable.

Hardware adapters pass because they **are** a DualShock 3 in the console's eyes:
VID `054C:0268`, DS3 HID descriptor and report format, adopted by the firmware's
own driver. No plugin can do that.

### What works: answering the question from inside the game

If the game asks "is this controller official?" and we are inside its process,
we can answer. `nativepadps3_game.sprx` intercepts `cellPadGetInfo2` and reports
`device_type = 0` with full conformity.

That buys the same result as the adapter, without hardware. It is what the
PS3xPAD "compatibility mode" tried to achieve by patching the game; intercepting
the API is far less invasive.

And, in the same hook, `cellPadGetData` gets the analog stick curve correction.
It works on any controller that reaches the game, because it never touches
device identity.

---

## How the detour works

On PowerPC64 ELFv1 a function pointer **is** the address of a two-word
descriptor `{entry point, TOC}`. Every module that imports a function points to
the **same** descriptor in the exporting library. Overwriting those two words
redirects the whole process.

```c
uint32_t *opd = (uint32_t *)cellPadGetData;   /* the descriptor */
saved[0] = opd[0];  saved[1] = opd[1];
opd[0] = hook[0];   opd[1] = hook[1];        /* and that's it */
```

No walking tables, no NID, no dependence on where the process is loaded.
Calling the original means calling through `saved`, which is a valid descriptor.

**Confirmed by measurement:** the descriptor of `cellPadGetData` landed at
`0x00AA0728` and my module (9 KB) at `0x00C400C4`, about 1.6 MB apart, meaning
it is the shared descriptor inside `sys_io`, not a local thunk.

The first attempt walked the import table starting from
`*(uint32_t *)0x1008C + 0x984`, an offset inherited from the published VSH
plugins. That **only** resolves in the VSH process; in a game process 0x10000 is
the EBOOT's ELF. The range guards I had put in saved the console: they returned
empty instead of dereferencing garbage.

---

## Files

| File | Role |
|---|---|
| `gamehook.c` | **The core.** Detours of `cellPadGetInfo2` and `cellPadGetData` inside the game |
| `sticks.c` | Calibration and the DS3 response curve; see [ANALOG_STICKS.md](ANALOG_STICKS.md) |
| `hook.c` | Descriptor detour mechanism |
| `config.c` | Config file reading |
| `main.c` | PRX entry; game path and VSH path separated by `#ifdef` |
| `util.c` | Boot trace, CRC32, file reading, remapping |
| `libc.c` | Minimal runtime (`-nodefaultlibs`) |
| `padldd.c`, `usb.c`, `passthru.c`, `ds4.c`, `dualsense.c`, `diag.c` | Old path, VSH build only |

The game build compiles **7 files** and imports **2 libraries** (`sys_fs` and
threads). This is not cosmetic: a game process loads `libio` and `libfs` but
**not** `libusbd`, and a single import from it leaves the loader unable to
resolve the module, so PS3MAPI gets stuck on "Load" forever.

The auth-id also differs per build: `1070000052000001` (vsh/module) for the VSH
one, `1010000001000003` (retail game) for the game one. PS3xPAD's
`xpad_game.sprx` uses the same one, for the same reason.

---

## Known limits

**PS button in game: impossible by this path.** It is the VSH that handles that
button and draws the quick menu; the game never sees it. A hook inside the game
process is on the wrong layer. The VSH module resolves it in the XMB, via
`CELL_PAD_CTRL_LDD_PS` in the extra word of the LDD pad, and only there.

**The detour reaches the process, but depends on the game calling `sys_io`.** A
title that reads the pad by another path would not be captured. God of War II HD
lists `libio` among its modules and works.

**Timing.** The plugin comes in after the game has already started. If the game
does the controller check only once at startup, it may be necessary to force a
re-evaluation: unplug and replug the controller, or enter the controller menu.

**Vibration, sixaxis and touchpad** are not implemented by this path. The code
exists in the VSH build, inherited from the earlier architecture.
