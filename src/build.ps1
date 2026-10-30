param(
    [ValidateSet('vsh', 'game')][string]$Variant = 'vsh',
    [switch]$Debug,
    [string]$Sdk = $env:CELL_SDK,
    [string]$Tools = $env:DS45_TOOLS,
    [ValidateSet('auto', 'fself', 'scetool')][string]$Sign = 'auto'
)
# Native Windows equivalent of build.sh. Requires an independently provided
# Cell SDK; one without GetInfo2/SetPortSetting (192.001) gets the compat shim.
#
# Signing: 'scetool' signs with retail keys (runs on CEX CFW and HEN) and needs
# DS45_TOOLS. 'fself' uses the SDK's own make_fself: a fake-signed SELF meant
# for DEX/debug firmware (a retail console rejects it); it needs no keys and
# ignores DS45_TOOLS. 'auto' picks scetool when DS45_TOOLS is set, fself otherwise.
$ErrorActionPreference = 'Stop'
if (!$Sdk) { throw 'Set CELL_SDK. No SPRX was built.' }
if ($Sign -eq 'auto') { $Sign = if ($Tools) { 'scetool' } else { 'fself' } }
if ($Sign -eq 'scetool' -and !$Tools) { throw 'Set DS45_TOOLS (scetool plus data/keys) to sign with scetool. No SPRX was built.' }
$gcc = Join-Path $Sdk 'host-win32/ppu/bin/ppu-lv2-gcc.exe'
$strip = Join-Path $Sdk 'host-win32/bin/ppu-lv2-prx-strip.exe'
$include = Join-Path $Sdk 'target/ppu/include'
$lib = Join-Path $Sdk 'target/ppu/lib'
$deps = @($gcc,$strip,(Join-Path $include 'cell/pad.h'))
if ($Sign -eq 'scetool') {
    $signer = Join-Path $Tools 'scetool.exe'
    $deps += @($signer,(Join-Path $Tools 'data/keys'))
} else {
    $signer = Join-Path $Sdk 'host-win32/bin/make_fself.exe'
    $deps += $signer
}
foreach ($path in $deps) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing build dependency: $path" }
}
function Invoke-BuildTool([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Build tool failed ($LASTEXITCODE): $Program" }
}
$sources = @('libc','util','hook','config','sticks','diag','main')
$libs = @('-lio_stub','-lfs_stub','-llv2_stub')
$compat = Join-Path $PSScriptRoot '../patches/compat_pad.h'
$flags = @('-mprx','-Os','-ffunction-sections','-fdata-sections','-fno-builtin-printf','-nodefaultlibs','-std=gnu99','-Wall','-Wno-unused-parameter','-I',$include)
# The shim rebuilds cellPadGetInfo2/cellPadSetPortSetting for old SDKs (192.001)
# that lack them; an SDK that declares them (475.001) must not get it.
$libpad = Join-Path $include 'cell/pad/libpad.h'
$hasInfo2 = (Test-Path -LiteralPath $libpad) -and (Select-String -LiteralPath $libpad -Pattern 'cellPadGetInfo2' -Quiet)
if (!$hasInfo2 -and (Test-Path -LiteralPath $compat)) {
    $flags = @('-include', $compat) + $flags
}
# -Debug defines DS45_DEBUG (boot trace, LV2 dumper) and tags the output name.
$suffix = ''
if ($Debug) { $flags += '-DDS45_DEBUG'; $suffix = '_debug' }
if ($Variant -eq 'game') {
    $sources += 'gamehook'; $flags += '-DDS45_GAME'; $auth = '1010000001000003'
} else {
    $sources += @('ds4','dualsense','padldd','usb','passthru','lv2','kernel','btcheck')
    if ($Debug) { $sources += 'lv2dump', 'lv2capture' }   # debug VSH only; never release or game
    $libs = @('-lusbd_stub') + $libs; $auth = '1070000052000001'
}
$objdir = Join-Path $PSScriptRoot "obj/$Variant$suffix"
New-Item -ItemType Directory -Force -Path $objdir | Out-Null
$objects = @()
foreach ($name in $sources) {
    $object = Join-Path $objdir "$name.o"
    Invoke-BuildTool $gcc ($flags + @('-c',(Join-Path $PSScriptRoot "$name.c"),'-o',$object))
    $objects += $object
}
$sym = Join-Path $objdir 'ds45pad.sym'
$prx = Join-Path $objdir 'ds45pad.prx'
$signed = Join-Path $objdir 'ds45pad.signed.tmp'
$output = Join-Path $PSScriptRoot "ds45pad_$Variant$suffix.sprx"
Invoke-BuildTool $gcc (@('-mprx','-zgenprx','-zgenstub','-ffunction-sections','-fdata-sections') + $objects + @('-L',$lib) + $libs + @('-o',$sym))
Invoke-BuildTool $strip @('--strip-debug','--strip-section-header',$sym,'-o',$prx)
$oldPS3 = $env:PS3
try {
    # Remove only a known temporary file, so a failed signing run cannot pass
    # by reading a stale signature from an earlier build.
    if (Test-Path -LiteralPath $signed) { Remove-Item -LiteralPath $signed }
    if ($Sign -eq 'scetool') {
        $env:PS3 = Join-Path $Tools 'data'
        Push-Location (Split-Path $signer)
        try {
            Invoke-BuildTool $signer @('-0','SELF','-1','TRUE','-s','FALSE','-2','0A','-3',$auth,'-4','01000002','-5','APP','-6','0003004000000000','-A','0001000000000000','-e',$prx,$signed)
        } finally {
            Pop-Location
        }
    } else {
        # make_fself takes no auth id (loading on DEX is confirmed on console, Task 11).
        Invoke-BuildTool $signer @($prx,$signed)
    }
    $bytes = [IO.File]::ReadAllBytes($signed)
    if ($bytes.Length -lt 4 -or [BitConverter]::ToString($bytes,0,4) -ne '53-43-45-00') { throw 'Invalid SCE container' }
    if (Test-Path -LiteralPath $output) {
        Copy-Item -LiteralPath $output -Destination "$output.bak-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmssfff'))"
    }
    Move-Item -LiteralPath $signed -Destination $output -Force
    Get-FileHash -LiteralPath $output -Algorithm SHA256
    $how = if ($Sign -eq 'fself') { 'fake-signed with make_fself, DEX/debug firmware only' } else { 'scetool, retail keys' }
    Write-Output "Successfully built and signed $Variant SPRX ($how): $output"
} finally { $env:PS3 = $oldPS3 }
