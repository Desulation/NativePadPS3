# Test for tools/check_no_proprietary.ps1. Builds throwaway git repos and checks
# the guard's exit code and output. Run: pwsh tools/test_check_no_proprietary.ps1
$ErrorActionPreference = 'Stop'
$guard = Join-Path $PSScriptRoot 'check_no_proprietary.ps1'

# Marker strings are built by concatenation so this file does not trip the guard.
$sdkMarker = 'SCE ' + 'CONFIDENTIAL'

$failures = 0

function New-TempRepo {
    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ('guardtest_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    git -C $dir init -q -b main | Out-Null
    git -C $dir config user.email 'test@example.com' | Out-Null
    git -C $dir config user.name 'test' | Out-Null
    return $dir
}

function Invoke-Case {
    param(
        [string]$Name,
        [scriptblock]$Setup,
        [int]$ExpectExit,
        [string]$ExpectText
    )
    $repo = New-TempRepo
    try {
        & $Setup $repo
        $out = & pwsh -NoProfile -File $guard -Staged -Root $repo 2>&1 | Out-String
        $code = $LASTEXITCODE
        $ok = ($code -eq $ExpectExit)
        if ($ok -and $ExpectText) { $ok = $out.Contains($ExpectText) }
        if ($ok) {
            Write-Host "PASS: $Name"
        } else {
            Write-Host "FAIL: $Name (exit $code, expected $ExpectExit; expected text '$ExpectText')"
            Write-Host $out
            $script:failures++
        }
    } finally {
        Remove-Item -Recurse -Force $repo -ErrorAction SilentlyContinue
    }
}

function Add-File {
    param([string]$Repo, [string]$Rel, [byte[]]$Bytes)
    $full = Join-Path $Repo $Rel
    New-Item -ItemType Directory -Force -Path (Split-Path $full -Parent) | Out-Null
    [System.IO.File]::WriteAllBytes($full, $Bytes)
    # -f: some banned paths are git-ignored in the real repo's rules; stage regardless.
    git -C $Repo add -f -- $Rel | Out-Null
}

$text = { param($s) [System.Text.Encoding]::UTF8.GetBytes($s) }

Invoke-Case 'clean repo with one .c file' {
    param($r) Add-File $r 'main.c' (& $text "int main(void){return 0;}`n")
} 0 ''

Invoke-Case 'staged .sprx is blocked' {
    param($r) Add-File $r 'x.sprx' (& $text 'abc')
} 1 'BLOCKED: x.sprx: banned extension'

Invoke-Case 'staged tools/dumps/ is blocked' {
    param($r) Add-File $r 'tools/dumps/a.txt' (& $text 'abc')
} 1 'banned path'

Invoke-Case 'staged kernel_cache.txt is blocked' {
    param($r) Add-File $r 'kernel_cache.txt' (& $text 'abc')
} 1 'banned path'

Invoke-Case 'staged SDK marker text is blocked' {
    param($r) Add-File $r 'notes.txt' (& $text "header $sdkMarker footer`n")
} 1 'SDK marker'

Invoke-Case 'staged binary not allowlisted is blocked' {
    param($r) Add-File $r 'x.dat' ([byte[]](0x41, 0x00, 0x42, 0x43))
} 1 'binary not allowlisted'

if ($failures -gt 0) {
    Write-Host "$failures case(s) failed"
    exit 1
}
Write-Host 'All guard tests passed'
exit 0
