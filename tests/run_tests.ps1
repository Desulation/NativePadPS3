# Native host test runner. Needs only LLVM clang; no PS3 SDK, no Node.
#   pwsh tests/run_tests.ps1 [-Filter <substring of test name>]
# Each tests/test_*.c is built with tests/fakes.c plus the src files named on
# its first line:   /* SOURCES: a.c b.c */   (relative to src/, may be empty)
# An optional second line adds compiler flags:   /* CFLAGS: -DNPP_DEBUG */
param([string]$Filter = '')
$ErrorActionPreference = 'Stop'

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (!$clang) { $clang = 'C:/Program Files/LLVM/bin/clang.exe' }
$repo = Split-Path $PSScriptRoot
$bin  = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$tests = Get-ChildItem -Path $PSScriptRoot -Filter 'test_*.c' | Sort-Object Name |
         Where-Object { $_.BaseName -like "*$Filter*" }
if (!$tests) { Write-Host "no tests match '$Filter'"; exit 1 }

$failed = 0
foreach ($t in $tests) {
    $name  = $t.BaseName
    $lines = Get-Content -Path $t.FullName -TotalCount 2
    $srcs = @()
    if ($lines.Count -ge 1 -and $lines[0] -match '^/\*\s*SOURCES:(.*?)\*/') {
        $srcs = @($Matches[1] -split '\s+' | Where-Object { $_ } |
                  ForEach-Object { Join-Path (Join-Path $repo 'src') $_ })
    }
    $extra = @()
    if ($lines.Count -ge 2 -and $lines[1] -match '^/\*\s*CFLAGS:(.*?)\*/') {
        $extra = @($Matches[1] -split '\s+' | Where-Object { $_ })
    }

    $exe = Join-Path $bin "$name.exe"
    if (Test-Path $exe) { Remove-Item $exe -Force }
    $ccArgs = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-DNPP_HOST_TEST', '-D_CRT_SECURE_NO_WARNINGS') + $extra +
            @('-I', (Join-Path $PSScriptRoot 'stubs'), '-I', (Join-Path $repo 'src'),
              '-I', $PSScriptRoot,
              $t.FullName, (Join-Path $PSScriptRoot 'fakes.c')) + $srcs +
            @('-o', $exe)
    & $clang @ccArgs
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL ${name}:build"; $failed++; continue }

    & $exe | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
exit ([int]($failed -gt 0))
