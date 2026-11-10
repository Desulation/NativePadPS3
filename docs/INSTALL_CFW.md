# Installing on CFW

The path is the same as for HEN, and simpler — **there is no HEN to re-enable on
every boot**. This document covers only the differences; the detailed
step-by-step is in [INSTALL_HEN.md](INSTALL_HEN.md).

---

## What stays the same

- The files go to `/dev_hdd0/plugins/nativepadps3/`
- webMAN MOD goes into `boot_plugins.txt`
- `nativepadps3_game.sprx` is loaded through PS3MAPI while the game is running
- The signature on my `.sprx` (key revision `0A`) works on both

## What changes

### 1. Nothing to re-enable

On CFW, `boot_plugins.txt` is read at boot and that's it. No browser exploit,
no activation app.

### 2. webMAN variant depends on the CFW

webMAN MOD has different builds. Pick the one that matches yours:

| Your CFW | Variant |
|---|---|
| Rebug (Cobra) | `webftp_server_rebug_cobra_ps3mapi.sprx` |
| Evilnat / other with Cobra | `webftp_server_cobra_ps3mapi.sprx` |
| CFW without Cobra | `webftp_server_ps3mapi.sprx` |
| PS3HEN | `webftp_server_ps3mapi.sprx` |

What matters is that it is a **`ps3mapi`** variant — that is the one that has
the function for loading a plugin into a game process. The names vary between
releases; check the file list of the release you download.

### 3. MAMBA uses a different file

If your setup runs MAMBA instead of Cobra, the plugin list is called
`mamba_plugins.txt`, not `boot_plugins.txt`. Same format, one path per
line.

### 4. The safety net still applies

On CFW a bad plugin in `boot_plugins.txt` can also hang the boot. The USB
stick trick works the same way: an **empty** `boot_plugins.txt` in the root of
a FAT32 USB stick takes priority over the one on the HDD.

Plan B: Recovery Menu → Rebuild Database, which deletes
`/dev_hdd0/boot_plugins.txt`.

CFW gives more room for error than HEN on module signing, but it is not
immune — set up the USB stick before testing.

---

## Recommended configuration

`/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/webftp_server_cobra_ps3mapi.sprx
/dev_hdd0/plugins/nativepadps3/nativepadps3_vsh.sprx
```

The second line is **optional** — it only makes the PS button work in the XMB.
If it makes no difference to you, leave just webMAN: fewer moving parts.

And in `nativepadps3.txt`, `INSERT_INTO_GAME = 0`. When enabled, it keeps apps
from loading, and it is no longer needed — compatibility comes from the in-game
hook.

---

## An advantage CFW has

With a **DEX kernel** and an EBOOT converted to debug, it is possible to load
`nativepadps3_game.sprx` automatically when the game boots, instead of through
PS3MAPI each time. This is the mechanism PS3xPAD documents as
`AUTO_GAME_LOAD`.

I have not implemented this and have not tested it — it is noted here because
it is the most relevant practical difference between CFW and HEN for this
project, in case you want to eliminate the manual PS3MAPI step.
