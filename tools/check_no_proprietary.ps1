<#
.SYNOPSIS
  Blocks proprietary / non-redistributable material from entering this repository.

.DESCRIPTION
  Checks the file list (staged files with -Staged, otherwise every tracked or
  untracked-but-not-ignored file) against:
    - banned extensions (.sprx .prx .sym .self .elf .a .bin .pup .pkg .rap .edat .o)
    - banned paths (tools/dumps/, any path segment named "keys", kernel_cache.txt, sdk_doc_chm/)
    - SDK copyright markers in file contents (case-sensitive)
    - binary files (NUL byte in the first 8000 bytes); the allowlist is empty

  Prints one "BLOCKED: <path>: <reason>" line per offence.
  Exit code 0 = clean, 1 = at least one offence.

.PARAMETER Staged
  Check only what is staged in the index (used by .githooks/pre-commit).

.PARAMETER Root
  Repository root. Defaults to the parent of this script's directory.
#>
param(
    [switch]$Staged,
    [string]$Root
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Root = (Resolve-Path -LiteralPath $Root).Path

# Marker strings are built by concatenation so this script never contains them literally.
$sdkMarkers = @(
    ('SCE ' + 'CONFIDENTIAL'),
    ('Sony Computer Entertainment Inc. ' + 'All Rights Reserved'),
    ('Sony Interactive Entertainment Inc. ' + 'All Rights Reserved')
)
$bannedExtensions = @('.sprx', '.prx', '.sym', '.self', '.elf', '.a', '.bin', '.pup', '.pkg', '.rap', '.edat', '.o')
$bannedPathPrefixes = @('tools/dumps/', 'sdk_doc_chm/')
$bannedSegments = @('keys', 'kernel_cache.txt')
$binaryAllowlist = @()   # intentionally empty
$binaryProbeBytes = 8000

function Invoke-Git {
    param([string[]]$GitArgs)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'git'
    $psi.WorkingDirectory = $Root
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.ArgumentList.Add('-c'); $psi.ArgumentList.Add('core.quotepath=false')
    foreach ($a in $GitArgs) { $psi.ArgumentList.Add($a) }
    $p = [System.Diagnostics.Process]::Start($psi)
    $ms = New-Object System.IO.MemoryStream
    $p.StandardOutput.BaseStream.CopyTo($ms)
    $null = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw "git $($GitArgs -join ' ') failed (exit $($p.ExitCode))" }
    return ,$ms.ToArray()
}

function Get-PathList {
    if ($Staged) {
        $bytes = Invoke-Git @('diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z')
    } else {
        $bytes = Invoke-Git @('ls-files', '-co', '--exclude-standard', '-z')
    }
    $s = [System.Text.Encoding]::UTF8.GetString($bytes)
    return @($s -split "`0" | Where-Object { $_ -ne '' } | Sort-Object -Unique)
}

function Get-Content-Bytes {
    param([string]$RelPath)
    if ($Staged) { return ,(Invoke-Git @('show', ":$RelPath")) }
    $full = Join-Path $Root $RelPath
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { return $null }  # deleted / submodule
    return ,[System.IO.File]::ReadAllBytes($full)
}

$offences = New-Object System.Collections.Generic.List[string]

foreach ($path in (Get-PathList)) {
    $lower = $path.ToLowerInvariant()
    $segments = $lower -split '/'

    $ext = [System.IO.Path]::GetExtension($lower)
    if ($ext -and ($bannedExtensions -contains $ext)) {
        $offences.Add("BLOCKED: ${path}: banned extension ($ext)")
        continue
    }

    $pathBanned = $false
    foreach ($prefix in $bannedPathPrefixes) {
        if ($lower.StartsWith($prefix) -or $lower.Contains("/$prefix")) { $pathBanned = $true }
    }
    foreach ($seg in $segments) {
        if ($bannedSegments -contains $seg) { $pathBanned = $true }
    }
    if ($pathBanned) {
        $offences.Add("BLOCKED: ${path}: banned path")
        continue
    }

    $bytes = Get-Content-Bytes $path
    if ($null -eq $bytes) { continue }

    $probe = [Math]::Min($binaryProbeBytes, $bytes.Length)
    $isBinary = $false
    for ($i = 0; $i -lt $probe; $i++) {
        if ($bytes[$i] -eq 0) { $isBinary = $true; break }
    }
    if ($isBinary) {
        if ($binaryAllowlist -notcontains $path) {
            $offences.Add("BLOCKED: ${path}: binary not allowlisted")
        }
        continue
    }

    # Latin-1 maps bytes 1:1 to chars, so markers match regardless of source encoding.
    $text = [System.Text.Encoding]::Latin1.GetString($bytes)
    foreach ($m in $sdkMarkers) {
        if ($text.Contains($m, [System.StringComparison]::Ordinal)) {
            $offences.Add("BLOCKED: ${path}: SDK marker")
            break
        }
    }
}

if ($offences.Count -gt 0) {
    $offences | ForEach-Object { Write-Output $_ }
    exit 1
}
exit 0
