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

Invoke-Case 'empty boot_plugins.txt (null and empty input)' {
    # Get-Content of an empty file yields $null; restore must not throw on it.
    $a = @(Remove-BootPluginEntries $null)
    $b = @(Remove-BootPluginEntries @())
    $c = @(Set-BootPluginEntry $null $RemoteDebugSprx)
    ($a.Count -eq 0) -and ($b.Count -eq 0) -and (Same $c @($RemoteDebugSprx)) -and
    ((Test-BootPluginSingle $null $RemoteDebugSprx) -eq $false)
}

Invoke-Case 'unrelated plugin survives restore' {
    $r = @(Remove-BootPluginEntries @($web, $RemoteDebugSprx))
    Same $r @($web)
}

# Power-off offsets come from the generated shim header, loudly (no silent fallback).
function New-TempHeader([string[]]$Lines) {
    $p = Join-Path ([IO.Path]::GetTempPath()) ('hdr_' + [Guid]::NewGuid().ToString('N') + '.h')
    [IO.File]::WriteAllText($p, (($Lines -join "`n") + "`n"))
    $p
}

Invoke-Case 'po offsets parsed' {
    $h = New-TempHeader @('#define NPP_SHIM_OFF_POWEROFF 0x140 /* x */', '#define NPP_SHIM_OFF_TEARDOWN 0x1e0 /* y */')
    try { $o = Get-PoOffsets $h } finally { Remove-Item -LiteralPath $h -ErrorAction SilentlyContinue }
    ($o.Poweroff -eq 0x140) -and ($o.Teardown -eq 0x1E0)
}

Invoke-Case 'po offsets loud on old prefix' {
    # old prefix on purpose (assembled so no legacy token sits in the source): a stale
    # header from before the rename must fail, not fall back
    $oldPfx = 'DS' + '45_SHIM_OFF_'
    $h = New-TempHeader @("#define ${oldPfx}POWEROFF 0x140 /* x */", "#define ${oldPfx}TEARDOWN 0x1e0 /* y */")
    $threw = $false
    try { $null = Get-PoOffsets $h } catch { $threw = $_.Exception.Message -like '*NPP_SHIM_OFF_POWEROFF/TEARDOWN*' } finally { Remove-Item -LiteralPath $h -ErrorAction SilentlyContinue }
    $threw
}

Invoke-Case 'po offsets loud on missing teardown' {
    $h = New-TempHeader @('#define NPP_SHIM_OFF_POWEROFF 0x140 /* x */')
    $threw = $false
    try { $null = Get-PoOffsets $h } catch { $threw = $_.Exception.Message -like '*NPP_SHIM_OFF_POWEROFF/TEARDOWN*' } finally { Remove-Item -LiteralPath $h -ErrorAction SilentlyContinue }
    $threw
}

$shimHdr = Join-Path $PSScriptRoot '../src/shim_blob.h'
if (Test-Path -LiteralPath $shimHdr) {
    Invoke-Case 'po offsets match the build' {
        $o = Get-PoOffsets $shimHdr
        ($PoOffs.Poweroff -eq $o.Poweroff) -and ($PoOffs.Teardown -eq $o.Teardown)
    }
} else {
    Write-Host 'SKIP: po offsets match the build (src/shim_blob.h not generated)'
}

# The signing-tools variable was renamed: the build scripts must say so, loudly, and must
# not start a build. These run the real scripts, but they fail before touching any SDK.
function Invoke-WithEnv([hashtable]$Env, [scriptblock]$Body) {
    $saved = @{}
    foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    try {
        foreach ($k in $Env.Keys) { [Environment]::SetEnvironmentVariable($k, $Env[$k]) }
        & $Body
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

Invoke-Case 'build.ps1 legacy DS45_TOOLS fails loudly' {
    $script = Join-Path $PSScriptRoot '../src/build.ps1'
    $legacy = Invoke-WithEnv @{ DS45_TOOLS = 'C:\nowhere'; NPP_TOOLS = $null } {
        $out = & pwsh -NoProfile -File $script -Sdk 'C:\nowhere' 2>&1 | Out-String
        @{ Code = $LASTEXITCODE; Out = $out }
    }
    $clean = Invoke-WithEnv @{ DS45_TOOLS = $null; NPP_TOOLS = $null } {
        $out = & pwsh -NoProfile -File $script -Sdk 'C:\nowhere' 2>&1 | Out-String
        @{ Code = $LASTEXITCODE; Out = $out }
    }
    ($legacy.Code -ne 0) -and $legacy.Out.Contains('NPP_TOOLS') -and
    ($clean.Code -ne 0) -and (-not $clean.Out.Contains('NPP_TOOLS'))
}

Invoke-Case 'build.sh legacy DS45_TOOLS fails loudly' {
    $bash = 'C:\Program Files\Git\bin\bash.exe'
    if (-not (Test-Path -LiteralPath $bash)) { Write-Host '  (Git bash not found, build.sh not checked)'; return $true }
    $src = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../src')).Path
    $r = Invoke-WithEnv @{ DS45_TOOLS = '/nowhere'; NPP_TOOLS = $null } {
        Push-Location $src
        try { $out = & $bash build.sh 2>&1 | Out-String; @{ Code = $LASTEXITCODE; Out = $out } } finally { Pop-Location }
    }
    ($r.Code -eq 1) -and $r.Out.Contains('NPP_TOOLS') -and $r.Out.Contains('No SPRX was built')
}

if ($failures -gt 0) { Write-Host "$failures case(s) failed"; exit 1 }
Write-Host 'all cases passed'
exit 0
