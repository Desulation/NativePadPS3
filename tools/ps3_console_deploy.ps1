<#
.SYNOPSIS
    Deploy the debug VSH plugin and a config to a DEX console running webMAN MOD,
    reboot it and show whether the kernel shim installed.

.DESCRIPTION
    Backs up the console's plugin, config and boot_plugins.txt into -Dest, uploads
    the debug build (size-checked) and the config, adds the plugin to
    /dev_hdd0/boot_plugins.txt if it is not listed, renames the old boot trace,
    reboots through webMAN and waits for the plugin's loop. Then prints the kshim
    trace lines, the last shimdbg counters line and the four hook slots (four
    throttled peeks). Nothing on the console is deleted. -Dest is console data:
    keep it under tools/dumps/ (git-ignored).

    Follow with ps3_console_linkkeys.ps1 (flip the DualSense link-key types, pads
    OFF), power the pads on, then ps3_console_trace.ps1 to read the counters.
    ps3_console_restore.ps1 puts the console back to stock.

.EXAMPLE
    pwsh tools/ps3_console_deploy.ps1 -Ip 192.168.1.97 -Dest tools/dumps/dex492/run1
.EXAMPLE
    pwsh tools/ps3_console_deploy.ps1 -Ip 192.168.1.97 -Dest tools/dumps/dex492/run2 -Config 'ENABLE = 1','ENABLE_SHIM = 1','SHIM_STABLE_ID = 0'
#>
param(
    [Parameter(Mandatory)][string]$Ip,
    [Parameter(Mandatory)][string]$Dest,
    [string]$Sprx = (Join-Path $PSScriptRoot '../src/ds45pad_vsh_debug.sprx'),
    [string[]]$Config = @('ENABLE = 1', 'ENABLE_SHIM = 1', 'SHIM_INPUT_HOOK = 1', 'SHIM_EVENT_HOOK = 1',
                          'SHIM_ENABLE_HOOK = 1', 'SHIM_INLINE_PID = 0', 'SHIM_STABLE_ID = 1', 'PROBE_EXEC = 0',
                          'DUMP_LV2 = 0')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')
$remoteSprx = '/dev_hdd0/plugins/ds45pad_vsh_debug.sprx'

if (!(Test-Path -LiteralPath $Sprx)) { throw "No plugin build at $Sprx (run src/build.ps1 -Variant vsh -Debug)." }
if (!(Test-Port 21)) { throw "No FTP server on ${Ip}:21 (is the console on, with webMAN MOD?)." }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')

# 1. back up what is replaced
Curl-Ok @('-o', (Join-Path $Dest "prev_$stamp.sprx"), "$ftp$remoteSprx")
Curl-Ok @('-o', (Join-Path $Dest "prev_ds45pad_$stamp.txt"), "$ftp/dev_hdd0/plugins/ds45pad/ds45pad.txt")
$bp = Join-Path $Dest "prev_boot_plugins_$stamp.txt"
Curl-Ok @('-o', $bp, "$ftp/dev_hdd0/boot_plugins.txt")
Say ('boot_plugins.txt now: ' + ((Get-Content $bp) -join ' | '))
Say ('ds45pad.txt now: ' + ((Get-Content (Join-Path $Dest "prev_ds45pad_$stamp.txt")) -join ' | '))

# 2. upload the plugin (size-checked) and the config
Curl-Ok @('-T', $Sprx, "$ftp$remoteSprx")
$local = (Get-Item -LiteralPath $Sprx).Length
if (-not ((& curl.exe -s -m 30 "$ftp/dev_hdd0/plugins/") | Where-Object { "$_" -match "\s$local\s.*ds45pad_vsh_debug\.sprx$" })) {
    throw "Uploaded plugin size does not match $local bytes."
}
Say "Uploaded plugin ($local bytes)"
$cfg = Join-Path $Dest 'ds45pad.txt'
[IO.File]::WriteAllText($cfg, ($Config -join "`n") + "`n")
Curl-Ok @('-T', $cfg, "$ftp/dev_hdd0/plugins/ds45pad/ds45pad.txt")
Say ('Uploaded config: ' + ($Config -join ' | '))

# 3. boot_plugins.txt: append the plugin if it is not listed
$cur = @(Get-Content $bp | Where-Object { $_.Trim() })
if (-not ($cur -contains $remoteSprx)) {
    $new = Join-Path $Dest 'boot_plugins.new.txt'
    [IO.File]::WriteAllText($new, (($cur + $remoteSprx) -join "`n"))
    Curl-Ok @('-T', $new, "$ftp/dev_hdd0/boot_plugins.txt")
    $chk = Join-Path $Dest 'boot_plugins.check.txt'
    Curl-Ok @('-o', $chk, "$ftp/dev_hdd0/boot_plugins.txt")
    if (-not ((Get-Content $chk) -contains $remoteSprx)) { throw 'boot_plugins.txt does not list the plugin after the upload' }
    Say ('boot_plugins.txt now: ' + ((Get-Content $chk) -join ' | '))
}

# 4. a fresh trace, then reboot
if ((& curl.exe -s -m 30 "$ftp/dev_hdd0/tmp/") -match 'ds45pad_boot\.txt') {
    Curl-Ok @('-Q', 'RNFR /dev_hdd0/tmp/ds45pad_boot.txt', '-Q', "RNTO /dev_hdd0/tmp/ds45pad_boot_$stamp.txt", "$ftp/")
    Say "Old trace renamed to ds45pad_boot_$stamp.txt"
}
Say 'Rebooting (webMAN /reboot.ps3)'
& curl.exe -s -m 10 "http://$Ip/reboot.ps3" | Out-Null
Wait-Reboot
$t = [DateTime]::Now
while ($true) {
    $tr = $null; try { $tr = Get-Trace $Dest 'boot.txt' } catch { }
    if ($tr -and ($tr -match '(?i)^loop')) { break }
    if (([DateTime]::Now - $t).TotalMinutes -gt 3) { throw 'plugin did not reach its loop within 3 min' }
    Start-Sleep 5
}
Say ("Plugin in its loop. Trace lines: {0}" -f $tr.Count)
$tr | Where-Object { $_ -match '(?i)kshim|kmatch|gated|exec_probe|^loop|cfg_shim_stable_id' } | Select-Object -First 20 | ForEach-Object { "   $_" }
$dbg = $tr | Where-Object { $_ -match 'shimdbg' } | Select-Object -Last 1
if ($dbg) { "   LAST $dbg" } else { '   (no shimdbg line yet)' }
'   event  0x37CFF8  = ' + (Hex (Peek-Row '37CFF0') 8 8) + '  (stock 800000000033A2B0)'
'   enable 0x37CFB0  = ' + (Hex (Peek-Row '37CFB0') 0 8) + '  (stock 800000000033A310)'
'   input  0x37CE38  = ' + (Hex (Peek-Row '37CE30') 8 8) + '  (stock 800000000033A010)'
'   startup 0x37CFE8 = ' + (Hex (Peek-Row '37CFE0') 8 8) + '  (always 800000000033A208)'
