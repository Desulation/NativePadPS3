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

# Decode a PowerPC I-form branch (primary opcode 18). $word is the 32-bit instruction
# as 8 hex digits (big-endian, as Hex prints it), $at its address. Returns @{ Kind; Target }
# with Kind one of b / bl / ba / bla (AA = absolute, LK = link), Target the absolute
# destination (AA = 0 is relative to $at), or $null if the word is not such a branch.
# Only the destination is ever compared: no instruction bytes of the stock kernel are
# kept in these scripts.
function Decode-Branch([string]$word, [int64]$at) {
    if ($word -notmatch '^[0-9A-Fa-f]{8}$') { return $null }
    $w = [Convert]::ToUInt32($word, 16)
    if (($w -shr 26) -ne 18) { return $null }
    $li = [int64]($w -band 0x03FFFFFC)
    if ($li -band 0x02000000) { $li -= 0x04000000 }
    $aa = [bool]($w -band 2)
    $lk = [bool]($w -band 1)
    $kind = 'b' + $(if ($aa) { 'a' } else { '' })
    if ($lk) { $kind = 'bl' + $(if ($aa) { 'a' } else { '' }) }
    @{ Kind = $kind; Target = $(if ($aa) { $li } else { $at + $li }) }
}

# The kernel shim's entry points for the power-off hooks, as offsets from the blob base.
# Read from the generated src/shim_blob.h when it is present (so they follow the build);
# otherwise the DEX 4.92 build's values.
$ExecBase = 0x57C000
$PoOffs = @{ Poweroff = 0x130; Teardown = 0x1D0 }
$blobHdr = Join-Path $PSScriptRoot '../src/shim_blob.h'
if (Test-Path -LiteralPath $blobHdr) {
    $hdr = Get-Content -LiteralPath $blobHdr
    foreach ($kv in @(@('Poweroff', 'POWEROFF'), @('Teardown', 'TEARDOWN'))) {
        $m = $hdr | Select-String -Pattern ("DS45_SHIM_OFF_{0}\s+0x([0-9A-Fa-f]+)" -f $kv[1]) | Select-Object -First 1
        if ($m) { $PoOffs[$kv[0]] = [Convert]::ToInt32($m.Matches[0].Groups[1].Value, 16) }
    }
}

# The four power-off code sites (DEX 4.92): the instruction at each is a branch; the shim
# redirects it to its blob, and the stock destination is what a restored kernel has.
# Sites 1-3 are only hooked in observe mode.
$PoSites = @(
    @{ Site = 0xCA9BC; Shim = 'Poweroff'; Stock = 0x102750; Observe = $false },
    @{ Site = 0xF4D94; Shim = 'Teardown'; Stock = 0x1174FC; Observe = $true },
    @{ Site = 0xF722C; Shim = 'Teardown'; Stock = 0x1174FC; Observe = $true },
    @{ Site = 0xFDB44; Shim = 'Teardown'; Stock = 0x1174FC; Observe = $true }
)

# Read back the four power-off code sites (four throttled peeks, one per 16-byte row; the
# 32-bit word is bytes (site mod 16)..+3 of the row at site - (site mod 16)) and print the
# branch destination of each. $State is 'shim' (hook on: first site -> the poweroff entry,
# the others -> the teardown entry in observe mode, or still stock in act mode) or 'stock'
# (every destination is the original). Returns the number of unexpected sites.
function Show-PoSites([ValidateSet('shim', 'stock')][string]$State) {
    $bad = 0
    foreach ($s in $PoSites) {
        $row = $s.Site -band (-bnot 0xF)
        $off = $s.Site -band 0xF
        $b = Peek-Row ('{0:X6}' -f $row)
        $label = '   po site 0x{0:X}' -f $s.Site
        if (!$b) { Write-Host "$label = (no reply)"; $bad++; continue }
        $d = Decode-Branch (Hex $b $off 4) $s.Site
        if (!$d) { Write-Host "$label = not a branch"; $bad++; continue }
        $t = $d.Target
        $desc = '{0} 0x{1:X}' -f $d.Kind, $t
        $shimT = $ExecBase + $PoOffs[$s.Shim]
        if ($State -eq 'stock') {
            if ($t -eq $s.Stock) { $v = 'stock' } else { $v = 'NOT STOCK (stock 0x{0:X})' -f $s.Stock; $bad++ }
        } elseif ($t -eq $shimT) {
            $v = if ($s.Observe) { 'hook (observe mode)' } else { 'hook' }
        } elseif ($t -eq $s.Stock -and $s.Observe) {
            $v = 'stock (expected only in act mode; hook would be 0x{0:X})' -f $shimT
        } else {
            $v = 'UNEXPECTED (hook 0x{0:X}, stock 0x{1:X})' -f $shimT, $s.Stock; $bad++
        }
        Write-Host "$label = $desc  $v"
    }
    $bad
}

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
