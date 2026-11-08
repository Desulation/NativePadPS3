# Offline self-test for tools/ps3_console_lib.ps1: the boot_plugins.txt helpers and the
# console file names. It never contacts a console (dot-sourcing the lib only builds the
# $ftp string). Run: pwsh -File tools/test_console_lib.ps1
$ErrorActionPreference = 'Stop'
$Ip = 'offline.invalid'
. (Join-Path $PSScriptRoot 'ps3_console_lib.ps1')

# legacy on-console name, read-only compatibility, do not rename
$old = '/dev_hdd0/plugins/ds45pad_vsh_debug.sprx'
$oldRel = '/dev_hdd0/plugins/ds45pad/ds45pad_vsh.sprx'
$web = '/dev_hdd0/plugins/webftp_server.sprx'

$failures = 0

function Assert-Case {
    param([string]$Name, [bool]$Ok)
    if ($Ok) { Write-Host "PASS: $Name" }
    else { Write-Host "FAIL: $Name"; $script:failures++ }
}

function Same([string[]]$a, [string[]]$b) {
    $a = @($a); $b = @($b)
    ($a.Count -eq $b.Count) -and (($a -join "`n") -ceq ($b -join "`n"))
}

function Invoke-Case {
    param([string]$Name, [scriptblock]$Body)
    $ok = $false
    try { $ok = [bool](& $Body) } catch { Write-Host "  error: $($_.Exception.Message)" }
    Assert-Case $Name $ok
}

Invoke-Case 'replace old debug entry' {
    $r = @(Set-BootPluginEntry @($web, $old) $RemoteDebugSprx)
    (Same $r @($web, $RemoteDebugSprx)) -and ((Test-BootPluginSingle $r $RemoteDebugSprx) -eq $true)
}

Invoke-Case 'replace old release entry' {
    $r = @(Set-BootPluginEntry @($oldRel, $web) $RemoteDebugSprx)
    (Same $r @($web, $RemoteDebugSprx)) -and ((Test-BootPluginSingle $r $RemoteDebugSprx) -eq $true)
}

Invoke-Case 'old and new both listed, new twice' {
    $r = @(Set-BootPluginEntry @($old, $RemoteDebugSprx, $web, $RemoteDebugSprx) $RemoteDebugSprx)
    Same $r @($web, $RemoteDebugSprx)
}

Invoke-Case 'commented old line and blank lines' {
    $r = @(Set-BootPluginEntry @('', "#$old", $web, '   ') $RemoteDebugSprx)
    Same $r @($web, $RemoteDebugSprx)
}

Invoke-Case 'empty file' {
    $r = @(Set-BootPluginEntry @() $RemoteDebugSprx)
    Same $r @($RemoteDebugSprx)
}

Invoke-Case 'single check rejects' {
    ((Test-BootPluginSingle @($old) $RemoteDebugSprx) -eq $false) -and
    ((Test-BootPluginSingle @($RemoteDebugSprx, $RemoteDebugSprx) $RemoteDebugSprx) -eq $false) -and
    ((Test-BootPluginSingle @($web) $RemoteDebugSprx) -eq $false) -and
    ((Test-BootPluginSingle @("$RemoteDebugSprx  ") $RemoteDebugSprx) -eq $true)
}

Invoke-Case 'restore strips both names' {
    $r = @(Remove-BootPluginEntries @($old, $web, $RemoteDebugSprx, $oldRel))
    $e = @(Remove-BootPluginEntries @($old))
    (Same $r @($web)) -and ($e.Count -eq 0)
}

Invoke-Case 'trace name matches the plugin' {
    $hdr = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../src/npp.h')
    $m = $hdr | Select-String -Pattern '^#define\s+NPP_BOOT_FILE\s+"([^"]+)"' | Select-Object -First 1
    $m -and ($m.Matches[0].Groups[1].Value -ceq $TraceFile)
}

Invoke-Case 'debug plugin name matches the build' {
    $build = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '../src/build.ps1')
    ((Split-Path -Leaf $RemoteDebugSprx) -ceq 'nativepadps3_vsh_debug.sprx') -and
    $build.Contains('"nativepadps3_$Variant$suffix.sprx"')
}

if ($failures -gt 0) { Write-Host "$failures case(s) failed"; exit 1 }
Write-Host 'all cases passed'
exit 0
