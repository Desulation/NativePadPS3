param(
    [ValidateSet('vsh', 'game')][string]$Variant = 'vsh',
    [switch]$Debug,
    [string]$Sdk = $env:CELL_SDK,
    [string]$Tools = $env:NPP_TOOLS,
    [ValidateSet('auto', 'fself', 'scetool')][string]$Sign = 'auto'
)
# Native Windows equivalent of build.sh. Requires an independently provided
# Cell SDK; one without GetInfo2/SetPortSetting (192.001) gets the compat shim.
#
# Signing: 'scetool' signs with retail keys (runs on CEX CFW and HEN) and needs
# NPP_TOOLS. 'fself' uses the SDK's own make_fself: a fake-signed SELF meant
# for DEX/debug firmware (a retail console rejects it); it needs no keys and
# ignores NPP_TOOLS. 'auto' picks scetool when NPP_TOOLS is set, fself otherwise.
$ErrorActionPreference = 'Stop'
# legacy variable name, one-cycle migration check; remove after the next release
if ($env:DS45_TOOLS -and !$env:NPP_TOOLS) { throw 'DS45_TOOLS was renamed NPP_TOOLS: set NPP_TOOLS to the same folder (and unset DS45_TOOLS). No SPRX was built.' }
if (!$Sdk) { throw 'Set CELL_SDK. No SPRX was built.' }
if ($Sign -eq 'auto') { $Sign = if ($Tools) { 'scetool' } else { 'fself' } }
if ($Sign -eq 'scetool' -and !$Tools) { throw 'Set NPP_TOOLS (scetool plus data/keys) to sign with scetool. No SPRX was built.' }
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
# The kernel shim blob is a separate freestanding PPC64 image the VSH plugin
# embeds. It is deliberately NOT built with the Cell SDK's ppu-lv2-gcc (that
# toolchain is ILP32 and would silently truncate the kernel's 64-bit pointers);
# it is built true-LP64 big-endian with LLVM - clang + ld.lld - and packed into
# src/shim_blob.h by tools/shim_pack.py. Only the VSH variants build it.
function Build-ShimBlob([string]$Repo) {
    $clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
    if (!$clang) { $clang = 'C:/Program Files/LLVM/bin/clang.exe' }
    $lld = (Get-Command ld.lld -ErrorAction SilentlyContinue).Source
    if (!$lld) { $lld = 'C:/Program Files/LLVM/bin/ld.lld.exe' }
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (!$py) { $py = (Get-Command py -ErrorAction SilentlyContinue).Source }
    foreach ($p in @($clang, $lld, $py)) {
        if (!$p -or !(Test-Path -LiteralPath $p)) {
            throw "The VSH shim blob needs LLVM clang, ld.lld and python (missing: $p)."
        }
    }
    $res = Join-Path ((& $clang -print-resource-dir).Trim()) 'include'
    $shim = Join-Path $Repo 'shim'
    $src  = Join-Path $Repo 'src'
    # LP64 big-endian PPC64, freestanding, position-independent via relocation
    # (not PIC codegen): see Task 4's verified invocation and the task-5 report.
    $cflags = @('--target=powerpc64-unknown-linux-gnu', '-std=c99', '-O2',
                '-ffreestanding', '-fno-builtin', '-nostdinc', '-isystem', $res,
                '-fno-jump-tables', '-fno-stack-protector', '-fno-pic',
                '-mcmodel=small', '-Wall', '-Wextra')
    $units = @(
        @{ src = (Join-Path $shim 'entry.c');      obj = (Join-Path $shim 'entry.o') },
        @{ src = (Join-Path $shim 'shim.c');       obj = (Join-Path $shim 'shim.o') },
        @{ src = (Join-Path $shim 'ds3_report.c'); obj = (Join-Path $shim 'ds3_report.o') },
        @{ src = (Join-Path $shim 'ds3_output.c'); obj = (Join-Path $shim 'ds3_output.o') },
        @{ src = (Join-Path $src  'sixaxis.c');    obj = (Join-Path $shim 'sixaxis.o') },
        @{ src = (Join-Path $src  'ds_body.c');    obj = (Join-Path $shim 'ds_body.o') }
    )
    $objs = @()
    foreach ($u in $units) {
        Invoke-BuildTool $clang ($cflags + @('-c', $u.src, '-o', $u.obj))
        $objs += $u.obj
    }
    $elf = Join-Path $shim 'shim.elf'
    Invoke-BuildTool $lld (@('-m', 'elf64ppc', '-T', (Join-Path $shim 'shim.ld'),
                             '--emit-relocs', '--no-dynamic-linker', '-static') +
                           $objs + @('-o', $elf))
    Invoke-BuildTool $py @('-I', (Join-Path $Repo 'tools/shim_pack.py'), $elf,
                           (Join-Path $src 'shim_blob.h'))
    Write-Output "Regenerated src/shim_blob.h from the freestanding kernel shim blob."
}
$sources = @('libc','util','hook','config','paths','sticks','diag','main')
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
# -Debug defines NPP_DEBUG (boot trace, LV2 dumper) and tags the output name.
$suffix = ''
if ($Debug) { $flags += '-DNPP_DEBUG'; $suffix = '_debug' }
if ($Variant -eq 'game') {
    $sources += 'gamehook'; $flags += '-DNPP_GAME'; $auth = '1010000001000003'
} else {
    # sixaxis + ds_body are VSH-only: the DualSense body decode and sixaxis
    # scaling feed dualsense/padldd, neither of which the game variant links.
    $sources += @('ds4','dualsense','ds_body','sixaxis','padldd','usb','passthru','lv2','kernel','btcheck','kpatch','kshim','kshim_table')
    if ($Debug) { $sources += 'lv2dump', 'lv2capture' }   # debug VSH only; never release or game
    $libs = @('-lusbd_stub') + $libs; $auth = '1070000052000001'
    Build-ShimBlob (Split-Path $PSScriptRoot)   # regenerate src/shim_blob.h before the plugin compiles
}
$objdir = Join-Path $PSScriptRoot "obj/$Variant$suffix"
New-Item -ItemType Directory -Force -Path $objdir | Out-Null
$objects = @()
foreach ($name in $sources) {
    $object = Join-Path $objdir "$name.o"
    Invoke-BuildTool $gcc ($flags + @('-c',(Join-Path $PSScriptRoot "$name.c"),'-o',$object))
    $objects += $object
}
$sym = Join-Path $objdir 'nativepadps3.sym'
$prx = Join-Path $objdir 'nativepadps3.prx'
$signed = Join-Path $objdir 'nativepadps3.signed.tmp'
$output = Join-Path $PSScriptRoot "nativepadps3_$Variant$suffix.sprx"
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
