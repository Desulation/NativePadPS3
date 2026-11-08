<#
.SYNOPSIS
    Put the console back to stock: take the plugin out of /dev_hdd0/boot_plugins.txt
    (the other entries stay), reboot, and confirm the kernel hooks are original.

.DESCRIPTION
    The shim lives in kernel RAM only, so a reboot without the plugin in
    boot_plugins.txt removes it. After the reboot this reads the start-up event slot
    and the product-ID helper word (two peeks) and the four power-off code sites (four
    more peeks; only each branch destination is decoded and compared) and reports
    whether they are stock.
    The plugin and its config stay on the disk, ready for the next deploy. Backups of
    the old boot_plugins.txt go to -Dest.

.EXAMPLE
    pwsh tools/ps3_console_restore.ps1 -Ip 192.168.1.97 -Dest tools/dumps/dex492/run1
#>
param(
    [Parameter(Mandatory)][string]$Ip,
    [Parameter(Mandatory)][string]$Dest
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
if (!(Test-Port 21)) { throw 'console FTP not reachable' }

$cur = Join-Path $Dest 'boot_plugins.before.txt'
Curl-Ok @('-o', $cur, "$ftp/dev_hdd0/boot_plugins.txt")
Say ('before: ' + ((Get-Content $cur) -join ' | '))
$keep = @(Get-Content $cur | Where-Object { $_.Trim() -and $_ -notmatch 'ds45pad' })
$new = Join-Path $Dest 'boot_plugins.after.txt'
[IO.File]::WriteAllText($new, ($keep -join "`n"))
Curl-Ok @('-T', $new, "$ftp/dev_hdd0/boot_plugins.txt")
$chk = Join-Path $Dest 'boot_plugins.check.txt'
Curl-Ok @('-o', $chk, "$ftp/dev_hdd0/boot_plugins.txt")
$after = Get-Content $chk
Say ('after:  ' + ($after -join ' | '))
if ($after -match 'ds45pad') { throw 'the plugin is still listed in boot_plugins.txt' }

Say 'Rebooting (webMAN /reboot.ps3)'
& curl.exe -s -m 10 "http://$Ip/reboot.ps3" | Out-Null
Wait-Reboot
Start-Sleep 20   # let the VSH settle
$ev = Hex (Peek-Row '37CFF0') 8 8
"   event slot 0x37CFF8 = $ev  (stock 800000000033A2B0)"
'   helper     0x29E2C8 = ' + (Hex (Peek-Row '29E2C0') 8 8) + '  (stock 4E8000206880042F)'
Say 'Power-off code sites (every destination should be the original)'
$badSites = Show-PoSites 'stock'
if ($ev -eq '800000000033A2B0' -and $badSites -eq 0) { Say 'CLEAN: no shim installed, the hooks are stock.' }
elseif ($ev -ne '800000000033A2B0') { Say "WARNING: event slot is not stock ($ev)" }
if ($badSites -ne 0) { Say "WARNING: $badSites power-off code site(s) are not stock" }
