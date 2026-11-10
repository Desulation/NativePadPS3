# Diagnostic mode

There are two things that no documentation answers and only the console does:
where the PS button shows up (if it shows up at all) on the Bluetooth path, and
how the DS4/DualSense sixaxis axes correspond to those of the DualShock 3.

Diagnostic mode answers both.

## Turning it on

In `nativepadps3.txt`:

```
DIAG = 1
```

Restart the plugin. Everything goes to `/dev_hdd0/tmp/nativepadps3_diag.txt`;
pull it over the webMAN FTP. Every time a report code changes, a line is
written.

It samples `cellPadGetData()` **and** `cellPadGetRawData()`, because the raw
variant sometimes carries codes that the normal one discards, and that is
exactly where the PS button may be hiding.

Turn it off afterwards: the file grows quickly.

---

## Finding the PS button on Bluetooth

1. Pair the controller and send it to port 7 (`Reassign Controllers`).
2. `DIAG = 1`, restart the plugin.
3. Press **only the PS button**, once, and wait two seconds.
4. Open the file.

The port list comes out first and says which is which:

```
--- ports ---
max=7 connected=2
port 0  status=0x0001  setting=0x0006  cap=0x001F  type=5  [CONFORMING]
port 6  status=0x0001  setting=0x0006  cap=0x0000  type=0  [GENERIC]
```

`[GENERIC]` is your DS4/DualSense. `[CONFORMING]` is the virtual controller
created by the plugin; ignore it.

Now look for what changed when you pressed PS:

```
data port 6:  [2]=0x0100
```

That is a find: code **2**, bit **0x0100**. Put it in `nativepadps3.txt`:

```
BT_PS_CODE = 2
BT_PS_MASK = 0x0100
BT_PS_COMBO = NONE
```

A real PS button on Bluetooth, with no combination.

**If nothing shows up** when you press PS, the firmware really does discard the
button and there is nothing to be done on the software side; stick with
`BT_PS_COMBO`. This is the most likely result, but it is worth the thirty
seconds of testing.

The same procedure works for the touchpad click and for the DualSense Edge
paddles.

---

## Calibrating the sixaxis

It only makes sense over USB (over Bluetooth there is no motion at all).

The plugin already converts the magnitude correctly: the DS4/DualSense report
8192 units per g, and the DualShock 3 uses 113 around the center 512. What may
be wrong is the **direction** of each axis, which is not documented.

Quick test, without reading any file: pick a game with obvious sixaxis (any one
with a tilt minigame; the *flOw* or *Super Stardust HD* menus work well) and
watch:

| Symptom | Fix in `nativepadps3.txt` |
|---|---|
| Tilting left moves right | `INVERT_X = 1` |
| Tilting forward moves backward | `INVERT_Y = 1` |
| Rotating the controller responds backwards | `INVERT_G = 1` |
| Tilting sideways responds as if it were forward | `SWAP_XY = 1` |
| Reacts too little | Raise `ACCEL_SCALE` (e.g. `150`) |
| Reacts too much / jitters | Lower `ACCEL_SCALE` (e.g. `70`) |

`GYRO_SCALE` is the most likely to need adjustment, because the DS3's
`SENSOR_G` field has no clean physical equivalent to the gyroscope of the newer
controllers; the current value is a deliberate estimate.

---

## Finding the VID/PID of an unofficial controller

Turn on `DIAG = 1`, plug in the controller and look in the file. If the plugin
did not recognize it, it does not appear; in that case use a PC (`lsusb` on
Linux, Device Manager on Windows) and add the line to
`nativepadps3_devices.txt`:

```
0x054c, 0x0ce6, DUALSENSE
```

Choose `DS4` or `DUALSENSE` according to the report format the clone imitates;
if in doubt, try both, as the wrong one simply produces no input at all.

---

## Nothing works

In order:

1. **No notice in the XMB at startup** → the plugin did not load. Check the
   path in `boot_plugins.txt` and whether the SPRX was signed.
2. **Notice appears, controller does nothing over USB** → another controller
   plugin may have grabbed the device first. Unload PS3xPAD if it is installed.
3. **Works in the XMB, not in the game** → syscalls 573/574 are not available;
   confirm that Cobra/Mamba is active.
4. **A specific game refuses the controller** → try `CAPABILITY_MASK = 0xFFFF`.
5. **Two controllers responding at the same time (Bluetooth)** → the physical
   controller still has to be reassigned to port 7.
