# Credits

## Original project

This repository is an **independent continuation** of
[domyno6773/DualSense-PS3](https://github.com/domyno6773/DualSense-PS3)
(commit `33ba29e`, v2.0.0), which was imported via the maintainer's fork
[Desulation/DualSense-PS3](https://github.com/Desulation/DualSense-PS3).
The original author's work is the foundation of everything here, and the GPLv2
`LICENSE` and all existing source headers are carried over unchanged.

If you want to support the original author, their donation link is
[paypal.me/domyno6773](https://www.paypal.com/paypalme/domyno6773).

## Why this is an independent continuation (fresh history)

1. **Scope.** The planned changes (rebuilding the Bluetooth support so it works
   on any firmware, among others) are too broad to be practical as an upstream
   contribution.
2. **Sony IP risk.** The upstream git history contains SDK-linked, retail-signed
   `.sprx` binaries. Those carry Sony intellectual-property risk. Starting from
   a fresh history, with the binaries removed and a guard that prevents them
   from ever being committed again (`tools/check_no_proprietary.ps1`), avoids
   carrying that risk forward.

## Upstream credits carried over

* **[Dobridp](https://github.com/Dobridp)** - author of the original
  [DS45Pad](https://github.com/Dobridp/DS45Pad) project.
* **[OsirisX](https://github.com/OsirisX)** - author of
  [PS3xPAD](https://github.com/OsirisX/PS3xPAD), the foundation for custom
  controller LDD injection on PS3.
* **[aldostools](https://github.com/aldostools)** - webMAN MOD, PRX signing
  guides, documentation of the PS3 VSH environment, and the source of the
  `vshmain` NIDs used here.
* **PS3Xploit Team** - PS3HEN and LV2 syscall research.
