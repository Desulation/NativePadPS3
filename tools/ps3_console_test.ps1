<#
.SYNOPSIS
    Deploy the debug VSH plugin to a console running webMAN MOD, reboot it,
    and collect the results of one on-console test.

.DESCRIPTION
    Uses webMAN MOD's FTP server and its /reboot.ps3 page. The console needs
    the plugin path in boot_plugins.txt. The person at the console still has
    to connect the Bluetooth pad and use it.

    -Mode dump     DUMP_LV2 = 1 in ds45pad.txt. Waits for LV2 dumps A and B
                   (follow the toasts: A untouched, B with Cross held and the
                   left stick fully left) and downloads them.
    -Mode btcheck  Follows the boot trace until the passive cross-check
                   passes or fails (use the pad: sticks and buttons), and
                   downloads the trace.

    Nothing on the console is deleted: files this run must start without are
    renamed with a timestamp suffix, and the plugin being replaced is
    downloaded to the destination folder first.

    Dumps and traces are the developer's own console data and must never be
    committed; tools/dumps/ is git-ignored.

.EXAMPLE
    pwsh tools/ps3_console_test.ps1 -Mode dump -Ip 192.168.1.97 -Dest tools/dumps/dex492
.EXAMPLE
    pwsh tools/ps3_console_test.ps1 -Mode btcheck -Ip 192.168.1.97 -Dest tools/dumps/dex492
#>
param(
    [Parameter(Mandatory)][ValidateSet('dump', 'btcheck')][string]$Mode,
    [Parameter(Mandatory)][string]$Ip,
    [Parameter(Mandatory)][string]$Dest,
    [string]$Sprx = (Join-Path $PSScriptRoot '../src/ds45pad_vsh_debug.sprx'),
    [string]$RemoteSprx = '/dev_hdd0/plugins/ds45pad_vsh_debug.sprx',
    [int]$TimeoutMin = 15
)
$ErrorActionPreference = 'Stop'
$ftp = "ftp://$Ip"
$tmp = '/dev_hdd0/tmp'
$stamp = [DateTime]::Now.ToString('yyyyMMdd-HHmmss')
$dumpFiles = @('ds45pad_lv2.bin', 'ds45pad_lv2.txt', 'ds45pad_lv2_b.bin', 'ds45pad_lv2_b.txt')
$trace = 'ds45pad_boot.txt'

function Say([string]$m) { Write-Output ("[{0}] {1}" -f [DateTime]::Now.ToString('HH:mm:ss'), $m) }

function Invoke-Curl([string[]]$CurlArgs) {
    $out = & curl.exe -s -S -m 120 @CurlArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "curl $($CurlArgs -join ' ') failed ($LASTEXITCODE): $out" }
    $out
}

function Test-Port([int]$Port) {
    $c = New-Object Net.Sockets.TcpClient
    try { return ($c.ConnectAsync($Ip, $Port).Wait(1500) -and $c.Connected) } catch { return $false } finally { $c.Close() }
}

# Name -> size for the files in /dev_hdd0/tmp (FTP LIST, unix style).
function Get-TmpListing {
    $sizes = @{}
    foreach ($line in (Invoke-Curl @("$ftp$tmp/"))) {
        $f = "$line" -split '\s+'
        if ($f.Count -ge 9) { $sizes[$f[-1]] = [int64]$f[4] }
    }
    $sizes
}

function Rename-Remote([string]$name) {
    $new = $name -replace '(\.[a-z]+)$', "_$stamp`$1"
    Invoke-Curl @('-Q', "RNFR $tmp/$name", '-Q', "RNTO $tmp/$new", "$ftp/") | Out-Null
    Say "Renamed $tmp/$name to $new"
}

function Get-Remote([string]$name, [string]$as = $name) {
    Invoke-Curl @('-o', (Join-Path $Dest $as), "$ftp$tmp/$name") | Out-Null
}

if (!(Test-Path -LiteralPath $Sprx)) { throw "No plugin build at $Sprx (run src/build.ps1 -Variant vsh -Debug)." }
if (!(Test-Port 21)) { throw "No FTP server on $Ip`:21 (is the console on, with webMAN MOD?)." }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# 1. Keep a copy of the plugin being replaced, then upload the new one.
$backup = Join-Path $Dest "prev_$stamp.sprx"
Invoke-Curl @('-o', $backup, "$ftp$RemoteSprx") | Out-Null
Say "Backed up $RemoteSprx to $backup"
Invoke-Curl @('-T', $Sprx, "$ftp$RemoteSprx") | Out-Null
$local = (Get-Item -LiteralPath $Sprx).Length
$dir = ($RemoteSprx -replace '/[^/]+$', '') + '/'
$leaf = $RemoteSprx -replace '^.*/', ''
if (-not ((Invoke-Curl @("$ftp$dir")) | Where-Object { "$_" -match "\s$local\s.*\s$([regex]::Escape($leaf))$" })) {
    throw "Uploaded plugin size does not match $local bytes."
}
Say "Uploaded $Sprx ($local bytes) to $RemoteSprx"

# 2. Start clean: a fresh boot trace, and for a dump run no earlier dumps.
$before = Get-TmpListing
$names = @($trace)
if ($Mode -eq 'dump') { $names += $dumpFiles }
foreach ($name in $names) { if ($before.ContainsKey($name)) { Rename-Remote $name } }

# 3. Reboot through webMAN and wait for the console to go down and come back.
Say 'Rebooting the console (webMAN /reboot.ps3)'
& curl.exe -s -m 10 "http://$Ip/reboot.ps3" | Out-Null
$t = [DateTime]::Now
while (Test-Port 21) {
    if (([DateTime]::Now - $t).TotalSeconds -gt 90) { throw 'Console did not go down within 90 s of the reboot request.' }
    Start-Sleep -Seconds 2
}
Say 'Console is down'
while (!(Test-Port 21)) {
    if (([DateTime]::Now - $t).TotalMinutes -gt 5) { throw 'Console did not come back within 5 min.' }
    Start-Sleep -Seconds 3
}
$deadline = [DateTime]::Now.AddMinutes($TimeoutMin)

if ($Mode -eq 'dump') {
    Say 'Console is back. Turn on the DualSense and follow the toasts.'
    $seen = @{}
    while ($true) {
        $now = Get-TmpListing
        foreach ($name in $dumpFiles) {
            if ($now.ContainsKey($name) -and -not $seen.ContainsKey($name)) { $seen[$name] = 1; Say "Appeared: $name" }
        }
        $done = ($now['ds45pad_lv2.bin'] -eq 8388608) -and ($now['ds45pad_lv2_b.bin'] -eq 8388608) -and
                $now.ContainsKey('ds45pad_lv2.txt') -and $now.ContainsKey('ds45pad_lv2_b.txt')
        if ($done) { break }
        if ([DateTime]::Now -gt $deadline) { Say "Timed out after $TimeoutMin min; downloading what is there."; break }
        Start-Sleep -Seconds 5
    }
    # Dump B is complete once its size is final; give the plugin a moment to
    # write the trace.
    Start-Sleep -Seconds 5
    $now = Get-TmpListing
    foreach ($name in $dumpFiles + $trace) {
        if ($now.ContainsKey($name)) { Get-Remote $name; Say "Downloaded $name ($($now[$name]) bytes)" }
        else { Say "Missing on console: $name" }
    }
    return
}

# btcheck: follow the trace. The plugin traces kmatch once, then the
# cross-check state on every change (0 pending, 1 passed, 2 failed) and the
# VSH sample count on every rescan.
Say 'Console is back. Turn on the DualSense and use it: move both sticks, press buttons.'
$out = "ds45pad_boot_btcheck_$stamp.txt"
$last = @{}
$result = $null
while ($true) {
    if ((Get-TmpListing).ContainsKey($trace)) {
        Get-Remote $trace $out
        $lines = Get-Content -LiteralPath (Join-Path $Dest $out)
        foreach ($key in 'kmatch', 'kernel_slot_lo', 'kernel_report_off', 'passthru_bound_port', 'btcheck', 'btcheck_vsh_samples', 'bt_work_lo',
                         'vsh_reads', 'vsh_ports', 'vsh_src_reads', 'vsh_src_changes', 'vsh_src_len', 'vsh_src_d1d2lxly', 'vsh_lock_busy', 'raw_samples') {
            $hit = $lines | Where-Object { $_ -like "$key=*" } | Select-Object -Last 1
            if ($hit -and $last[$key] -ne $hit) { $last[$key] = $hit; Say $hit }
        }
        if ($lines -contains 'btcheck_FAILED_stepping_aside') { $result = 'FAILED'; break }
        if ($last['btcheck'] -eq 'btcheck=0x00000001') { $result = 'PASSED'; break }
        if ($last['btcheck'] -eq 'btcheck=0x00000002') { $result = 'FAILED'; break }
    }
    if ([DateTime]::Now -gt $deadline) { $result = "no result within $TimeoutMin min"; break }
    Start-Sleep -Seconds 5
}
Say "Cross-check: $result (trace saved as $out)"
