<#
.SYNOPSIS
    Deploy the debug VSH plugin and a config to a DEX console running webMAN MOD,
    reboot it and show whether the kernel shim installed.

.DESCRIPTION
    Backs up the console's plugin, config (the new folder's, else the legacy one) and
    boot_plugins.txt into -Dest, uploads the debug build (size-checked) and the config
    (the config folder is created), makes /dev_hdd0/boot_plugins.txt list exactly one
    plugin line, the new one (any old or new plugin line is replaced; two listed
    plugins would install the kernel shim twice), renames the old boot trace,
    reboots through webMAN and waits for the plugin's loop. Then prints the kshim
    trace lines, the last shimdbg counters line, the four hook slots and the
    four power-off code sites with their branch destinations (eight throttled peeks;
    expected: the first site -> exec_base + the poweroff entry, the other three ->
    the teardown entry in observe mode, still original in act mode). Nothing on the console is deleted
    (old plugin files, the old config folder and old traces stay). -Dest is console data:
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
    [string]$Sprx = (Join-Path $PSScriptRoot '../src/nativepadps3_vsh_debug.sprx'),
    [string[]]$Config = @('ENABLE = 1', 'ENABLE_SHIM = 1', 'SHIM_INPUT_HOOK = 1', 'SHIM_EVENT_HOOK = 1',
                          'SHIM_ENABLE_HOOK = 1', 'SHIM_INLINE_PID = 0', 'SHIM_STABLE_ID = 1', 'PROBE_EXEC = 0',
                          'DUMP_LV2 = 0')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')
$remoteSprx = $RemoteDebugSprx

if (!(Test-Path -LiteralPath $Sprx)) { throw "No plugin build at $Sprx (run src/build.ps1 -Variant vsh -Debug)." }
if (!(Test-Port 21)) { throw "No FTP server on ${Ip}:21 (is the console on, with webMAN MOD?)." }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')

# 1. back up what is replaced
try { Curl-Ok @('-o', (Join-Path $Dest "prev_$stamp.sprx"), "$ftp$remoteSprx") }
catch { Say "No plugin at $remoteSprx yet (first deploy under this name); nothing to back up" }
$bp = Join-Path $Dest "prev_boot_plugins_$stamp.txt"
Curl-Ok @('-o', $bp, "$ftp/dev_hdd0/boot_plugins.txt")
Say ('boot_plugins.txt now: ' + ((Get-Content $bp) -join ' | '))
$prevCfg = Join-Path $Dest "prev_config_$stamp.txt"
$prevLegacyCfg = Join-Path $Dest "prev_legacy_config_$stamp.txt"
$backedUp = $null
try {
    Curl-Ok @('-o', $prevCfg, "$ftp$ConfigFile")
    $backedUp = $prevCfg
    Say 'backed up the new-folder config'
} catch {
    try {
        Curl-Ok @('-o', $prevLegacyCfg, "$ftp$LegacyConfigFile")
        $backedUp = $prevLegacyCfg
        Say 'backed up the legacy config'
    } catch {
        Say 'No config on the console (neither folder)'
    }
}
if ($backedUp) { Say ('config now: ' + ((Get-Content $backedUp) -join ' | ')) }

# 2. upload the plugin (size-checked) and the config
Curl-Ok @('-T', $Sprx, "$ftp$remoteSprx")
$local = (Get-Item -LiteralPath $Sprx).Length
if (-not ((& curl.exe -s -m 30 "$ftp/dev_hdd0/plugins/") | Where-Object { "$_" -match "\s$local\s.*$([regex]::Escape((Split-Path -Leaf $remoteSprx)))$" })) {
    throw "Uploaded plugin size does not match $local bytes."
}
Say "Uploaded plugin ($local bytes)"
$cfg = Join-Path $Dest 'nativepadps3.txt'
[IO.File]::WriteAllText($cfg, ($Config -join "`n") + "`n")
Curl-Ok @('--ftp-create-dirs', '-T', $cfg, "$ftp$ConfigFile")
Say ('Uploaded config: ' + ($Config -join ' | '))

# 3. boot_plugins.txt: replace (never append to) the plugin line. Two listed plugins would
#    install the kernel shim twice at boot, so the file must end with exactly one.
$cur = @(Get-Content $bp)
if (-not (Test-BootPluginSingle $cur $remoteSprx)) {
    $new = Join-Path $Dest 'boot_plugins.new.txt'
    [IO.File]::WriteAllText($new, (@(Set-BootPluginEntry $cur $remoteSprx) -join "`n"))
    Curl-Ok @('-T', $new, "$ftp/dev_hdd0/boot_plugins.txt")
}
$chk = Join-Path $Dest 'boot_plugins.check.txt'
Curl-Ok @('-o', $chk, "$ftp/dev_hdd0/boot_plugins.txt")
if (-not (Test-BootPluginSingle @(Get-Content $chk) $remoteSprx)) { throw 'boot_plugins.txt must list exactly one plugin line, the new one' }
Say ('boot_plugins.txt now: ' + ((Get-Content $chk) -join ' | '))

# 4. a fresh trace, then reboot
if ((& curl.exe -s -m 30 "$ftp/dev_hdd0/tmp/") -match [regex]::Escape((Split-Path -Leaf $TraceFile))) {
    Curl-Ok @('-Q', "RNFR $TraceFile", '-Q', "RNTO /dev_hdd0/tmp/nativepadps3_boot_$stamp.txt", "$ftp/")
    Say "Old trace renamed to nativepadps3_boot_$stamp.txt"
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
# the four power-off code sites: read back with four more throttled peeks
Say ('Power-off code sites (shim on; entries 0x{0:X} / 0x{1:X})' -f ($ExecBase + $PoOffs.Poweroff), ($ExecBase + $PoOffs.Teardown))
[void](Show-PoSites 'shim')
