# NativePadPS3 rename: console notes

Console: DEX 4.92 test console (debug VSH build, webMAN MOD). Plan: `docs/superpowers/plans/2026-11-08-nativepadps3-rename.md`. Raw traces and backups are kept out of git under `tools/dumps/dex492/rename1` and `rename2`. Pads are named by role only, never by address.

## Stage A: functional rename (2026-11-07 evening, before the mechanical `ds45` -> `npp` step)

Before: the console was stock (the previous power-off stage had been restored), `boot_plugins.txt` listed no plugin, the old plugin file and the old data folder (`/dev_hdd0/plugins/ds45pad/` with the old config and calibration files) were present, and the new data folder did not exist.

**Deploy** (`tools/ps3_console_deploy.ps1`, debug VSH build `nativepadps3_vsh_debug.sprx`, 114608 bytes): "first deploy under this name; nothing to back up", the old config was backed up from the legacy folder, the config was uploaded to the new folder (created by the upload), `boot_plugins.txt` ended with exactly one plugin line, the new one, the console rebooted and the plugin reached its loop. The power-off read-back (`Show-PoSites`) showed the close-send site hooked and the three teardown sites stock, as designed for act mode.

**Trace, new folder** (`start`, version, `datadir_new`, `config_bytes=0x000000F3`, `kshim_install`, `kshim_installed`, `loop`): no `config_NOT_FOUND_using_defaults`.

**Pad check, with a controller-arming slip of mine (recorded because it looks like a regression and is not):** only one of the two DualSenses had been armed, but the other one was the pad in use. The console then treated it as a generic pad, the PS button did nothing, and the shim trace showed only the DS3 (`lastpid` 0x0268, `inds` 0, `xl` 0). The link-key listing showed the used entry still at the generic type. After the pad was switched off and both entries armed, the DualSense came up through the DS3 route: PS button, input, LEDs, rumble and the XMB "Turn off the controller" all worked, and the trace showed DualSense input counters rising and `podbg` rewrites for the power-off requests. Lesson for the stages: arm every DualSense address up front, and check the link-key listing for the entry that is connected.

**Legacy fallback:** the new-folder config was hidden (renamed with the `.off` suffix) and the boot trace stamped, then the console rebooted: `start`, version, `datadir_legacy`, `config_bytes=0x000000F3` (the old 243-byte config), `kshim_installed`, `loop`. Both pads re-armed; the DualSense behaved as in the new-folder run.

**Back to the new folder:** the config was renamed back and the trace stamped again; after the reboot the trace shows `datadir_new` and the same config size.

**Restore** (`tools/ps3_console_restore.ps1`): the plugin was removed from `boot_plugins.txt` (the other two entries stayed), the console rebooted, all four power-off sites read stock (`bl 0x102750`, `bl 0x1174FC`, `b 0x1174FC`, `bl 0x1174FC`), event slot and helper word stock: `CLEAN`. The old plugin file, both config folders and all traces stay on the disk.

Not verified in stage A: nothing outstanding for step 1. The mechanical rename comes next, followed by stage B (deploy of the final build).

## Stage B: final build (2026-11-08 00:33 local)

Build under test: the final branch head 68984c4, debug VSH build `nativepadps3_vsh_debug.sprx`, 114608 bytes, on the same DEX 4.92 console.

Before: the console was stock (the previous stage had been restored), `boot_plugins.txt` listed no plugin, the new data folder was present with the config from stage A, and the old folder was present too.

**Deploy:** "backed up the new-folder config", the plugin and the config were uploaded, `boot_plugins.txt` then listed exactly one plugin line (the new path), the old trace was stamped `nativepadps3_boot_<stamp>.txt`, the console rebooted and the plugin reached its loop. The power-off read-back used the offsets parsed from `src/shim_blob.h` (entries 0x57C130 and 0x57C1D0): the close-send site was hooked and the three teardown sites stock (act mode, by design).

**Trace:** `start`, version, `datadir_new`, `config_bytes=0x000000F3`, `kshim_install`, `kshim_installed`, `loop`; no `config_NOT_FOUND`.

**Pads:** both DualSenses were armed up front (the lesson from stage A). The person at the console confirmed the DualSense worked: input through the DS3 route (the `inds` and `xl` counters rising; PS button, input), then turned it off from the XMB. `podbg` read `calls=2 ds=1 rew=1 pass=1 td=0`: one DualSense request rewritten once, one DS3 request passed through, no retries, no teardown.

**Restore:** the plugin was removed from `boot_plugins.txt` (the other entries stayed), the console rebooted, all four power-off sites read stock, event slot and helper word stock: `CLEAN`.

**Conclusion:** both console stages passed. The renamed final build loads, finds its config in the new folder, falls back to the old folder (stage A), and behaves as before.
