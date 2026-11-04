<#
.SYNOPSIS
    List the console's Bluetooth link-key table, or flip chosen pads to type 0x01
    (a Sony pad) so the PS3 routes them through its own DS3 driver.

.DESCRIPTION
    Without -Arm: read-only listing of every paired device (address, type,
    connected). With -Arm -Bd <address>[,<address>...]: for each listed address, find
    its entry and change the type byte 0x08 (generic pad) to 0x01 with one webMAN
    poke, then read it back. The pad must be OFF (a connected entry is refused), and
    the change lives in kernel RAM only: a reboot undoes it. Entries already at 0x01
    (a real DS3) are left alone; any other type is refused. A webMAN popup tells the
    person at the console to power the pads on.

    Addresses are 12 hex digits without separators, e.g. 4cb99b1324a8. They belong
    to your own controllers, so they are parameters and never stored in the repo.

.EXAMPLE
    pwsh tools/ps3_console_linkkeys.ps1 -Ip 192.168.1.97
.EXAMPLE
    pwsh tools/ps3_console_linkkeys.ps1 -Ip 192.168.1.97 -Arm -Bd a0ab51a6c913,4cb99b1324a8
#>
param(
    [Parameter(Mandatory)][string]$Ip,
    [switch]$Arm,
    [string[]]$Bd = @()
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')

if ($Arm -and $Bd.Count -eq 0) { throw '-Arm needs -Bd <address>[,<address>...]' }
# "pwsh -File" passes a comma list as one string, so split on commas here as well.
$want = @($Bd | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } |
          ForEach-Object { $_.Trim().ToLower() -replace '[:\-]', '' })
foreach ($w in $want) { if ($w -notmatch '^[0-9a-f]{12}$') { throw "bad address '$w' (12 hex digits)" } }

$entries = Get-LinkKeys -Only $(if ($Arm) { $want } else { @() })
if (-not $Arm) {
    foreach ($e in $entries) {
        $fl = [Convert]::ToUInt32((Hex $e.b 8 4), 16)
        'entry {0,2} @{1:X6}: BD {2}  type {3:X2}  connected {4}' -f $e.k, $e.row, $e.bd, ($fl -band 0xFF), [bool]($fl -band 0x1000)
    }
    return
}

foreach ($w in $want) { if (-not ($entries | Where-Object { $_.bd -eq $w })) { throw "no link-key entry for $w (pair it first: Settings > Accessory Settings)" } }
$done = 0
foreach ($e in $entries) {
    $fl = [Convert]::ToUInt32((Hex $e.b 8 4), 16)
    Say ('entry {0} ({1}) flags {2:X8}: type {3:X2}, connected={4}' -f $e.k, $e.bd, $fl, ($fl -band 0xFF), [bool]($fl -band 0x1000))
    if ($fl -band 0x1000) { throw "$($e.bd) is CONNECTED - turn the pad OFF first, then re-run" }
    if (($fl -band 0xFF) -eq 0x01) { Say '  already type 0x01, left alone'; continue }
    if (($fl -band 0xFF) -ne 0x08) { throw ('  flags {0:X8}: type is neither 0x08 nor 0x01, not touching it' -f $fl) }
    $new = ('{0:X8}' -f (($fl -band 0xFFFFFF00) -bor 0x01)) + (Hex $e.b 12 4)   # new flags u32 + the following 4 bytes unchanged
    & curl.exe -s -m 10 -o NUL "http://$Ip/poke.lv2?$('{0:X6}' -f ($e.row + 8))=$new"
    $nf = Hex (Peek-Row ('{0:X6}' -f $e.row)) 8 4
    if ($nf -ne $new.Substring(0, 8)) { throw "flip read back $nf, wanted $($new.Substring(0, 8))" }
    Say "  type flipped to 0x01 (read back $nf)"
    $done++
}
Say "$done entr$(if ($done -eq 1) { 'y' } else { 'ies' }) flipped. Power the pad(s) on now."
& curl.exe -s -m 10 -o NUL "http://$Ip/popup.ps3?$([uri]::EscapeDataString('DS45: power the pads on now'))"
