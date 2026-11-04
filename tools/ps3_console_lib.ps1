# Shared helpers for the on-console test scripts (ps3_console_*.ps1). Dot-source it
# AFTER setting $Ip (the console's address): the console must run webMAN MOD with its
# FTP server (port 21) and web server (port 80).
#
# Rules learned the hard way on the DEX 4.92 test console:
#   - never more than ~2-3 webMAN requests per second (a fast peek loop hung the console);
#     Peek-Row sleeps 400 ms before every request.
#   - read results from the on-disk trace over FTP, not from bursts of live peeks.
#   - a console stuck with no working controller: hold the physical power button.
# Nothing here is Sony material: it only talks to the developer's own console.

if (-not $Ip) { throw 'set $Ip (the console address) before dot-sourcing ps3_console_lib.ps1' }
$ftp = "ftp://$Ip"

function Say([string]$m) { Write-Host ("[{0}] {1}" -f [DateTime]::Now.ToString('HH:mm:ss'), $m) }

function Test-Port([int]$Port) {
    $c = New-Object Net.Sockets.TcpClient
    try { return ($c.ConnectAsync($Ip, $Port).Wait(1500) -and $c.Connected) } catch { return $false } finally { $c.Close() }
}

function Curl-Ok([string[]]$a) {
    $out = & curl.exe -s -S -m 60 @a 2>&1
    if ($LASTEXITCODE -ne 0) { throw "curl $($a -join ' ') failed ($LASTEXITCODE): $out" }
}

# 16 bytes (hex strings) of the 16-aligned LV2 row `$row` (6 hex digits), or $null.
function Peek-Row([string]$row) {
    Start-Sleep -Milliseconds 400
    $r = & curl.exe -s -m 10 "http://$Ip/peek.lv2?$row" 2>&1 | Out-String
    $pre = [regex]::Match($r, '(?s)<pre[^>]*>(.*?)</pre>').Groups[1].Value -replace '<br>', "`n" -replace '<[^>]+>', '' -replace '&nbsp;', ' '
    $m = [regex]::Match($pre, "(?i)0*$row\s+((?:[0-9A-F]{2} ){16})")
    if ($m.Success) { $m.Groups[1].Value.Trim() -split ' ' } else { $null }
}

function Hex([string[]]$b, [int]$from, [int]$n) { ($b[$from..($from + $n - 1)]) -join '' }

# Wait for the console to go down and come back after a webMAN /reboot.ps3.
function Wait-Reboot {
    $t = [DateTime]::Now
    while (Test-Port 21) { if (([DateTime]::Now - $t).TotalSeconds -gt 90) { throw 'console did not go down within 90 s' }; Start-Sleep 2 }
    Say 'Console is down'
    while (!(Test-Port 21)) { if (([DateTime]::Now - $t).TotalMinutes -gt 5) { throw 'console not back within 5 min' }; Start-Sleep 3 }
    Say 'Console is back'
}

# The plugin's on-disk boot trace (one FTP GET), saved to $Dest\$as and returned as lines.
function Get-Trace([string]$Dest, [string]$as) {
    Curl-Ok @('-o', (Join-Path $Dest $as), "$ftp/dev_hdd0/tmp/ds45pad_boot.txt")
    Get-Content -LiteralPath (Join-Path $Dest $as)
}

# The BT link-key table: 32 entries of 0x20 bytes at LV2 0x434910. Bytes 2..7 of an entry
# are the paired device's address, the u32 at +8 holds its flags (low byte = type: 0x01 a
# Sony pad, 0x08 generic; bit 0x1000 = connected). Returns @{ k; row; b } for each used
# entry (all of them, or only those whose address is in $Only).
function Get-LinkKeys([string[]]$Only = @()) {
    $found = @()
    for ($k = 0; $k -lt 32; $k++) {
        $row = 0x434910 + 0x20 * $k
        $b = Peek-Row ('{0:X6}' -f $row)
        if (!$b) { continue }
        $bd = (Hex $b 2 6).ToLower()
        if ($bd -eq '000000000000' -or $bd -eq 'ffffffffffff') { continue }
        if ($Only.Count -gt 0 -and ($Only -notcontains $bd)) { continue }
        $found += @{ k = $k; row = $row; b = $b; bd = $bd }
    }
    $found
}
