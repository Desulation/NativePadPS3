# DS3-route power-off: console notes

Console: DEX 4.92 test console (debug VSH build, webMAN MOD). Plan: `docs/superpowers/plans/2026-11-04-ds3-route-poweroff.md`. Raw boot traces are kept out of git under `tools/dumps/dex492/poweroff1`.

## Result in one paragraph

Observe stage: the XMB "Turn off the controller" reaches the close send at `0xCA9BC` for both pads, no teardown fires, and the DualSense refuses the DS3 report (six sends, lights stay on). Act stage: with `SHIM_POWEROFF_MODE = 1` the DualSense powers off on the first and only send (feature report `0x08`, CRC seed `0x53`, from `dualsensectl`, now confirmed on hardware), comes back with the same controller number, working input, rumble and LEDs, and the DS3 is untouched. Not verified on hardware: two DualSenses turned off at the same moment (the user's second DualSense is paired to a PC), and any other trigger of the same close request (console shutdown, user switch).

## Stage 1: observe (2026-11-07)

Config: the usual keys plus `SHIM_OUTPUT_HOOK = 1`, `SHIM_OUTPUT_MODE = 2`, `SHIM_POWEROFF_HOOK = 1`, `SHIM_POWEROFF_MODE = 0` (observe: record only, nothing rewritten, teardown observers installed). Pads: one Bluetooth DS3 and one DualSense armed by Bluetooth address (the Move motion controller untouched).

Install: the kernel shim installed on the first boot. The four power-off code sites read back as branches into the blob: `0xCA9BC` `bl` to blob entry `0x57C130`, `0xF4D94` `bl`, `0xF722C` `b` and `0xFDB44` `bl` to blob entry `0x57C1D0`. Nothing froze.

Idle (both pads connected and working, no menu action): `podbg` counters all zero, event ring empty. The teardown observers did not fire on ordinary Bluetooth traffic.

### "Turn off the controller" on the DS3

`podbg calls=1 ds=0 rew=0 pass=1 td=0`. One ring record, site 1 (close send): DualSense flag 0, handle 0, `r4` 3, report id `0xF4`, length 4, kind 3, payload `42 08 00 00`. This is the call shape the design predicted for `0xCA9BC`. No teardown record.

### "Turn off the controller" on the DualSense

`podbg calls=7 ds=6 rew=0 pass=7 td=0`: six new close-send records (seq 2 to 7), each site 1, DualSense flag 1, handle 1, `r4` 3, report id `0xF4`, length 4, kind 3, payload `42 08 00 00`, all passed through unchanged. No teardown record. What the user saw: the lights stayed on after choosing "Turn off the controller"; the pad did not power off.

The six records are the first send plus the kernel's five retries (it retries a close request that the pad refuses), so the DualSense refuses the DS3 report and the kernel gives up, as expected from the earlier probe. The DS3, by contrast, needed one send.

### Classification

Outcome A of the spec: the close send fires for both pads at the predicted site with the predicted arguments, and no link teardown fires for either (td stayed 0). The first open assumption is settled: the XMB "Turn off the controller" action reaches `0xCA9BC`, and for a DualSense it arrives with the DualSense pad flag set, so the act stage can key the rewrite on it. Not settled yet: whether the DualSense powers off on the feature report `0x08` (act stage). The `rew` counter should stay 1 per action in the act stage; a value of 5 or 6 for one action would mean the pad refused the new report too.

Restore: `tools/ps3_console_restore.ps1` removed the plugin from `boot_plugins.txt`, rebooted, and all four sites read back stock (`bl 0x102750`, `bl 0x1174FC`, `b 0x1174FC`, `bl 0x1174FC`); event slot and helper word stock.

## Stage 2: act (2026-11-07)

Config as stage 1 but `SHIM_POWEROFF_MODE = 1`. Install: the kernel shim installed on the first boot; only the close-send site `0xCA9BC` reads back patched (`bl` to blob entry `0x57C130`); the three teardown sites read stock, as designed for act mode. Idle with both pads connected: `podbg` mode 1, all counters zero, ring empty.

### "Turn off the controller" on the DualSense (act mode)

`podbg calls=1 ds=1 rew=1 pass=0 td=0`. One ring record: site 1, DualSense flag 1, handle 1, `r4` 3, report id `0xF4`, length 4, kind 3, payload `42 08 00 00` (the ring records the arguments at entry, before the rewrite). One send only, no retries (observe stage: six). What the user saw: the light bar went out and the pad disconnected. Pressing the PS button brought it back as the same controller number (#2) with working input, rumble in a game and the right LEDs.

### "Turn off the controller" on the DS3 (act mode)

`podbg calls=2 ds=1 rew=1 pass=1 td=0`: a second record with DualSense flag 0, handle 0, passed through unchanged. The DS3 powered off (LED off), reconnected without trouble and rumbles.

### Port swap

The user turned both pads off and reconnected the DualSense first, so the DualSense became #1 (blue light bar and player LED) and the DS3 #2; rumble worked on both. Turning the DualSense off again on its new port: a record with DualSense flag 1, handle 0, rewritten once. Final counters `calls=5 ds=3 rew=3 pass=2 td=0`: three DualSense turn-offs each rewritten once, two DS3 turn-offs passed through, no teardown at any point.

Restore: all four sites stock, event slot and helper word stock.
