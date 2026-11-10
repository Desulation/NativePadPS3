# Step-by-step installation — PS3HEN

HEN-specific guide. For CFW, see [INSTALL_CFW.md](INSTALL_CFW.md) — it is
the same path, without the step of re-enabling on every boot.

> **Final architecture:** what fixes compatibility and the analog sticks is
> **`nativepadps3_game.sprx`**, loaded into the game process through webMAN's
> PS3MAPI. `nativepadps3_vsh.sprx` is optional and only serves the PS button in
> the XMB. See [ARCHITECTURE.md](ARCHITECTURE.md).

## Summary of what to install

```
/dev_hdd0/plugins/webftp_server_ps3mapi.sprx     <- webMAN MOD (required)
/dev_hdd0/plugins/nativepadps3/nativepadps3_game.sprx      <- the one that does the work
/dev_hdd0/plugins/nativepadps3/nativepadps3.txt            <- configuration
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx       <- optional (PS button in the XMB)
```

`/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/webftp_server_ps3mapi.sprx
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx
```

With the game running: browser at the PS3's IP → **PS3MAPI** tab → **Game
Plugins** → select the game process → **Set** → in a free slot type
`/dev_hdd0/plugins/nativepadps3/nativepadps3_game.sprx` → **Load**.

Confirm in `/dev_hdd0/tmp/nativepadps3_boot.txt`:

```
game_mode
hook_cellPadGetData_ok
hook_cellPadGetDataExtra_ok
hook_cellPadGetInfo2_ok
```

To **quit the game** without the PS button: same PS3MAPI tab, **Exit** button.

---

The rest of this document is the detailed walkthrough, including the safety
net.

---

## ⚠️ Read this first

**On HEN, a plugin with a wrong signature makes the console hang at boot.** It
is not a permanent brick — there is a fix and the fix is in Step 1 — but you
don't want to find that out without the safety net in place.

The rule is simple: the `.sprx` **must** be signed with key revision
`0A` (3.55). This project's Makefile already does that. If you take a
ready-made `.sprx` from someone else, confirm it was signed that way.

---

## Step 0 — What you need to have

| Item | Note |
|---|---|
| PS3 with HEN active | Any recent version (4.8x / 4.9x) |
| `nativepadps3_vsh.sprx` built and signed | See Step 2 — this is the most laborious step |
| A FAT32 USB stick | For the safety net and for transferring files |
| A file manager on the PS3 | multiMAN, IRISMAN or webMAN MOD |

Having **webMAN MOD** installed makes things much easier, because it provides
FTP and you transfer everything from the PC without going back and forth with a
USB stick. It is not required.

---

## Step 1 — Set up the safety net (do this BEFORE anything else)

This step takes a minute and is what saves you if something goes wrong.

1. Take a FAT32 USB stick.
2. In its **root** (not inside any folder), create an **empty** text file named
   exactly `boot_plugins.txt`.
3. Keep that USB stick.

Why it works: the USB stick's `boot_plugins.txt` takes **priority** over the one
on the internal HDD. If the console hangs when you enable HEN, just plug in
that USB stick before enabling — no plugin loads, and you get in normally to
fix things.

**Plan B**, if you don't have the USB stick at hand: enter the **Recovery Menu**
(power off with the button, hold Power until the second beep, release, hold
again) and use **Rebuild Database**. This deletes `/dev_hdd0/boot_plugins.txt`.

---

## Step 2 — Get the built `.sprx`

**To be direct: I can't build this for you**, and this is the real hurdle of
the project.

A VSH plugin is a **PRX** — a relocatable module with a `.sceModuleInfo`
section, injected into a process that is already running — and not an ordinary
homebrew `.self`. Producing a PRX requires the `-mprx` mode, which only exists
in SCE's GCC. The open-source toolchain (`ps3toolchain`/PSL1GHT) has the whole
API the plugin needs, but **cannot emit this format**. The why, in detail, is
in [BUILD.md](BUILD.md#why-psl1ght-cannot-be-used-for-vsh-plugins).

So: the Cell SDK — which is the same toolchain **every** HEN plugin is built
with, not a special requirement of this project. It ships a native Windows
toolchain, so you need neither WSL nor to build anything from scratch.

**The step-by-step setup is in [BUILD.md](BUILD.md)**, with what to look for,
how to check the package is complete, the environment variables and the common
errors. After that it's just:

```bash
cd src
make
```

This produces `src/nativepadps3_vsh.sprx`.

If you'd rather not set up any environment, [REQUEST_A_BUILD.md](REQUEST_A_BUILD.md)
has a ready-made request to post on a forum.

Either way, **confirm the signing line**: it must have `-2 0A`.

---

## Step 3 — Copy the files to the console

Create the folder `/dev_hdd0/plugins/nativepadps3/` and put three files in it:

```
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx        <- src/nativepadps3_vsh.sprx
/dev_hdd0/plugins/nativepadps3/nativepadps3.txt             <- ps3_ready/plugins/nativepadps3/nativepadps3.txt
/dev_hdd0/plugins/nativepadps3/nativepadps3_devices.txt     <- ps3_ready/plugins/nativepadps3/nativepadps3_devices.txt
```

**Over FTP (webMAN MOD installed):** connect to the PS3's IP on port 21, blank
username and password, and drag the files over.

**Over USB stick:** copy the `nativepadps3` folder to the USB stick, plug it into
the PS3, and use multiMAN/IRISMAN to copy from `/dev_usb000/nativepadps3` to
`/dev_hdd0/plugins/nativepadps3`.

The two `.txt` files are optional — without them the plugin uses the defaults,
which are already what you want. But take `nativepadps3.txt` along anyway: that
is where the analog stick tuning lives.

---

## Step 4 — Register the plugin in `boot_plugins.txt`

1. Open (or create) the file `/dev_hdd0/boot_plugins.txt`.
2. Add **one line**, with the full path:

```
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx
```

3. Save.

Details that tend to cause trouble:

- One path per line, no quotes, no extra spaces.
- The file must be in the **root** of `dev_hdd0`, not inside `plugins/`.
- If you already use webMAN MOD, its line is already there. Just add yours
  below it, don't replace it.
- If an old `ds45pad` plugin line already exists, **replace** that line
  with the new one; never leave both, or the plugin is installed twice at
  boot. See "Upgrading from ds45pad" in the README.
- If your setup uses MAMBA instead of Cobra, the file is called
  `mamba_plugins.txt`.

---

## Step 5 — Reboot and enable HEN

1. Reboot the PS3.
2. Enable HEN the way you already do (the HEN Installer app or the browser
   exploit).
3. A few seconds later the notice **`NativePadPS3 Loaded`** should appear.

The notice is deliberately delayed by ~8 seconds, so it doesn't show up while
the XMB is still being assembled. The controller already works before that.

**If the console hangs here:** power off with the back button, plug in the USB
stick from Step 1, power on and enable HEN. No plugin will load. Then it is
almost certain that the `.sprx` was signed wrong — go back to Step 2 and check
the `-2 0A`.

---

## Step 6 — Connect the controller

### Over USB cable (recommended)

Plug it in and that's it. The light bar lights up in the port's color and the PS
button opens the XMB.

### Over Bluetooth

1. Connect the controller **by cable** first.
2. **Settings → Accessory Settings → Manage Bluetooth Devices →
   Register New Device → Start Scanning**.
3. While it scans, hold **PS + Share** (DS4) or **PS + Create** (DualSense)
   until the light flashes fast.
4. Accept the pairing and unplug the cable.
5. Still in **Accessory Settings → Reassign Controllers**, send the
   DS4/DualSense to **port 7**.

Step 5 is not optional: it frees port 1 for the virtual controller. Without
it, games see both controllers and the generic one (the bad one) wins.

Over Bluetooth the PS button comes through as **SELECT + START**, because the
firmware doesn't deliver the real PS button on that path.

---

## Step 7 — Calibrate the analog sticks (30 seconds, worth it)

With the controller connected, **rotate both analog sticks a few times all the
way to the stop**, making complete circles.

This teaches the plugin the real travel of each axis. It is what lets a Hall
effect stick — which often only goes from `0x14` to `0xEB` — reach the extreme.
The result is saved to `/dev_hdd0/plugins/nativepadps3/nativepadps3_cal.txt` and doesn't need to be redone.

Then test in a game. If the corners still lack reach, open
`nativepadps3.txt` and lower:

```
STICK_SATURATION = 75
```

Or `71`, which takes the diagonal to the absolute corner. The details are in
[ANALOG_STICKS.md](ANALOG_STICKS.md).

---

## If the console hangs when enabling HEN

Plug in the USB stick from Step 1 and enable HEN — no plugin loads and you get
in normally. Then read **`/dev_hdd0/tmp/nativepadps3_boot.txt`**.

The plugin writes a marker to that file **before** each step that touches the
system, closing the file after every write. The last marker present names the
step that hung:

| Last marker | Where it hung |
|---|---|
| *(file does not exist)* | The module never even ran — SPRX signature or format |
| `start` | Reading `nativepadps3.txt` |
| `config` | Reading `nativepadps3_cal.txt` |
| `sticks` | USB driver registration (`cellUsbdRegisterExtraLdd`) |
| `usb_init` | Still in USB registration |
| `usb_ok` | Installing the rumble detour |
| `rumble` | Entering the main loop |
| `loop` | It ran. If it hung afterwards, it is the pad injection or Bluetooth |
| `notify` | The lookup in the VSH import table — set `NOTIFY = 0` |

With the marker in hand, you can isolate it quickly. Two mitigations that almost
always help:

- `MODE = BT` in `nativepadps3.txt` skips the whole USB registration;
- `NOTIFY = 0` (already the default) avoids reading the import table, which
  depends on the firmware layout.

## If something doesn't work

| Symptom | Probable cause |
|---|---|
| Console hangs when enabling HEN | Wrong signature. USB stick from Step 1, and check `-2 0A` |
| No notice in the XMB | Wrong path in `boot_plugins.txt`, or the file is not in the root of `dev_hdd0` |
| Notice appears, controller doesn't respond over USB | Another controller plugin grabbed the device first. Remove PS3xPAD from `boot_plugins.txt` if it is there |
| Works in the XMB but not in the game | HEN didn't expose the pad syscalls. The plugin still works partially; the PS button remains good |
| A specific game rejects the controller | `CAPABILITY_MASK = 0xFFFF` in `nativepadps3.txt` |
| Two controllers responding together (Bluetooth) | The physical one hasn't been reassigned to port 7 (Step 6) |
| Analog sticks still short in the corners | Redo Step 7, then `STICK_SATURATION = 75` |

To investigate any of these, turn on `DIAG = 1` in `nativepadps3.txt` and read
`/dev_hdd0/tmp/nativepadps3_diag.txt` — the procedure is in
[DIAGNOSTICS.md](DIAGNOSTICS.md).

---

## Uninstall

Delete the line from `/dev_hdd0/boot_plugins.txt` and reboot. The
`plugins/nativepadps3/` folder can stay where it is with no effect.
