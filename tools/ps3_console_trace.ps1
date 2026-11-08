<#
.SYNOPSIS
    Fetch the plugin's on-disk boot trace (one FTP GET, no peeks) and show the shim
    lines and the last shimdbg counter lines.

.DESCRIPTION
    The debug plugin appends a shimdbg line to /dev_hdd0/tmp/ds45pad_boot.txt on every
    housekeeping tick, so the counters survive a network drop. Fields: ev/en/in = hook
    entries, inds = input reads of a DualSense, padmiss = input pad-lookup misses,
    comp/en2 = start-ups/enables completed, xl = reports translated, ps = reads passed,
    lastpid/lastvt = last pad seen by the input hook, idbd = identities taken from a
    Bluetooth address, idfb = completions that fell back to the counter, hnd = last
    connection handle looked up.

    Also printed: the last podbg line (calls/ds/rew/pass = power-off close-send hook
    entries, DualSense calls, calls rewritten, calls passed through; td = teardown hook
    entries; mode = the power-off stage) and every poevt line (seq=0x........ then the
    16-byte ring record as 32 hex digits; the record layout is in shim/shim_abi.h).

.EXAMPLE
    pwsh tools/ps3_console_trace.ps1 -Ip 192.168.1.97 -Dest tools/dumps/dex492/run1
#>
param(
    [Parameter(Mandatory)][string]$Ip,
    [Parameter(Mandatory)][string]$Dest,
    [int]$Tail = 12
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
if (!(Test-Port 21)) { Say 'console FTP not reachable'; return }
$tr = Get-Trace $Dest 'boot_now.txt'
Say ('trace lines: {0}' -f $tr.Count)
'--- kshim / exec_probe lines ---'
$tr | Where-Object { $_ -match '(?i)kshim|exec_probe|gated' } | Select-Object -First 10 | ForEach-Object { "   $_" }
"--- last $Tail shimdbg lines ---"
$tr | Where-Object { $_ -match 'shimdbg' } | Select-Object -Last $Tail | ForEach-Object { "   $_" }
'--- last podbg line (power-off hook counters) ---'
$tr | Where-Object { $_ -match '^podbg ' } | Select-Object -Last 1 | ForEach-Object { "   $_" }
'--- poevt lines (power-off event ring, oldest first) ---'
$tr | Where-Object { $_ -match '^poevt ' } | ForEach-Object { "   $_" }
