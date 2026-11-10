# Reference test: PS3xPAD 0.8 on HEN

The goal of this test: find out whether the problem is **my implementation** or
**your environment**. PS3xPAD is third-party code, proven to run on HEN, and it
uses the same PS button mechanism I use. If it works, the bug is mine and I fix
it. If not even it works, the mechanism doesn't do on your firmware what the
documentation says — and that is a different problem, which no code of mine can
solve.

---

## It is almost identical to what we already did

Same safety USB stick, same `boot_plugins.txt`, same FTP, same HEN
re-activation. **Only five things change:**

### 1. Take my plugin out of service first

In `/dev_hdd0/boot_plugins.txt`, **delete or comment out the NativePadPS3 line** (on older installs, the ds45pad line). Two
controller plugins fighting over the same device give meaningless results, and
invalidate the test.

You don't need to delete the files, just the line.

### 2. Different folder and files

```
/dev_hdd0/plugins/ps3xpad/xpad_vsh.sprx
/dev_hdd0/plugins/ps3xpad/xpad_game.sprx
/dev_hdd0/plugins/ps3xpad/xpad_settings.txt
/dev_hdd0/plugins/ps3xpad/xpad_devices.txt
/dev_hdd0/plugins/ps3xpad/xpad_remap.txt
/dev_hdd0/plugins/ps3xpad/font.png
/dev_hdd0/plugins/ps3xpad/background.png
```

The two `.png` files and `xpad_game.sprx` are not used in this test, but the
plugin expects to find them. Copy everything.

### 3. Different line in boot_plugins.txt

```
/dev_hdd0/plugins/ps3xpad/xpad_vsh.sprx
```

### 4. Native mode — the one that works with DualSense

PS3xPAD has no DualSense driver (the controller is much newer than it). But
**native mode** doesn't need one: it reads the generic pad the firmware already
created and re-injects it as an LDD pad. Exactly the same design as my
`MODE = BT`.

In `xpad_settings.txt`, it comes configured like this by default:

```
XPAD_MODE = 1
```

And then, **mandatory**: in **Settings → Accessory Settings → Reassign
Controllers**, send the DualSense to **port 7**.

Without this step native mode can't find the controller. It is the same reason
as in my Bluetooth mode: it frees the low port for the virtual pad.

### 5. Different button combinations

PS3xPAD has its own menu in the XMB:

| Combination | Function |
|---|---|
| START + SELECT + SQUARE | Opens the menu |
| START + SELECT + L3 | Unloads the plugin |
| START + SELECT + R3 | Moves the plugin into the game process |

The menu is useful: it shows what the plugin is detecting.

---

## What to observe

Two questions, and both answers matter:

**1. Does multiMAN open normally with PS3xPAD loaded?**

- **It opens** → its skeleton is stable on your console, and the stability
  problem is in my code. The way forward is to graft my modules onto its
  skeleton.
- **It doesn't open** → your environment has a problem with pad plugins in
  general, not with mine specifically. Nothing I write fixes that, and it is
  worth investigating the HEN setup.

**2. Does the DualSense PS button open the XMB?**

- **It opens** → the mechanism works on your firmware and the bug is mine. I
  fix it.
- **It doesn't open** → `CELL_PAD_CTRL_LDD_PS` doesn't do on your firmware what
  the documentation says. The PS button needs a different approach, and I stop
  insisting on this one.

An honest warning: PS3xPAD dates from 2015 and was never tested on 4.9x
firmware or with a DualSense. It may simply not load. **That is also a useful
result** — it means the incompatibility is due to its age, not my code.

---

## After the test

To go back to my plugin: switch the `boot_plugins.txt` line back and
reboot. The two must never be active at the same time.
