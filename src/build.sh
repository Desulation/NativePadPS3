#!/bin/bash
# DS45PAD - build script (Git Bash / MSYS2 on Windows)
#
# Reproduces the build exactly, without GNU make. The make.exe that ships in
# the webMAN MOD repo is linked as a GUI subsystem binary, so under Git Bash it
# returns no output and no usable exit code - calling the compiler directly is
# both simpler and easier to debug.
#
# Usage:  ./build.sh [game] [debug]
#   (no argument) -> ds45pad_vsh.sprx   (boot plugin, runs in the VSH)
#   game          -> ds45pad_game.sprx  (-DDS45_GAME; injected into a running
#                                        game to correct the stick curve)
#   debug         -> the same with -DDS45_DEBUG, named ds45pad_<variant>_debug.sprx
#                    (boot trace; the VSH build also gets the LV2 dumper)

set -e

SDK="${CELL_SDK:-C:/ps3sdk/cell}"
# DS45_TOOLS set: sign with scetool and retail keys (CEX CFW and HEN).
# Unset: the SDK's make_fself, a fake-signed SELF for DEX/debug firmware only.
TOOLS="${DS45_TOOLS:-}"

PPU_BIN="$SDK/host-win32/ppu/bin"
HOST_BIN="$SDK/host-win32/bin"
INCLUDE="$SDK/target/ppu/include"
LIBDIR="$SDK/target/ppu/lib"

# Auth id decides where the module is allowed to live.
#   1070000052000001 = sys/internal + vsh/module   -> the VSH build
#   1010000001000003 = retail game/update          -> the in-game build
# PS3xPAD signs its game plugin with the retail id for the same reason.
AUTH_VSH=1070000052000001
AUTH_GAME=1010000001000003

VARIANT=vsh
DEBUG=0
for a in "$@"; do
	case "$a" in
		game)  VARIANT=game ;;
		debug) DEBUG=1 ;;
	esac
done

if [ "$VARIANT" = "game" ]; then
	EXTRA="-DDS45_GAME"
	OUT="ds45pad_game.sprx"
	AUTH="$AUTH_GAME"
	# A game process loads libio and libfs but *not* libusbd, so the in-game
	# build must not contain a single reference to it - otherwise the loader
	# cannot resolve the module and PS3MAPI hangs on "Load" forever.
	SRCS="libc.c util.c hook.c config.c sticks.c gamehook.c diag.c main.c"
	LDLIBS="-lio_stub -lfs_stub"
else
	EXTRA=""
	OUT="ds45pad_vsh.sprx"
	AUTH="$AUTH_VSH"
	SRCS="libc.c util.c hook.c config.c ds4.c dualsense.c ds_body.c sixaxis.c \
	      sticks.c padldd.c usb.c passthru.c lv2.c kernel.c btcheck.c diag.c main.c"
	LDLIBS="-lusbd_stub -lio_stub -lfs_stub -llv2_stub"
fi

# debug: -DDS45_DEBUG (boot trace, and for the VSH build the LV2 dumper),
# output tagged _debug. Same switch as `make DEBUG=1` / `build.ps1 -Debug`.
if [ "$DEBUG" = "1" ]; then
	EXTRA="$EXTRA -DDS45_DEBUG"
	OUT="${OUT%.sprx}_debug.sprx"
	if [ "$VARIANT" = "vsh" ]; then
		SRCS="$SRCS lv2dump.c lv2capture.c"
	fi
fi

# The shim rebuilds cellPadGetInfo2/cellPadSetPortSetting for old SDKs (192.001)
# that lack them; an SDK that declares them (475.001) must not get it.
COMPAT_PAD="../patches/compat_pad.h"
EXTRA_INC=""
if [ -f "$COMPAT_PAD" ] && ! grep -q cellPadGetInfo2 "$INCLUDE/cell/pad/libpad.h" 2>/dev/null; then
	EXTRA_INC="-include $COMPAT_PAD"
fi

CFLAGS="-mprx -Os -ffunction-sections -fdata-sections -fno-builtin-printf \
        -nodefaultlibs -std=gnu99 -Wall -Wno-unused-parameter -I $INCLUDE $EXTRA_INC $EXTRA"

# -zgenprx/-zgenstub come from the SDK's own PRX rules; they are what turn the
# link output into a loadable module rather than an executable.
LDFLAGS="-mprx -zgenprx -zgenstub -ffunction-sections -fdata-sections \
         -Wl,--strip-unused-data"
# Key revision 0A (3.55) is mandatory for PS3HEN - an older revision makes the
# console hang on boot.
SCEFLAGS="-0 SELF -1 TRUE -s FALSE -2 0A -3 $AUTH -4 01000002 \
          -5 APP -6 0003004000000000 -A 0001000000000000"

# Tools are called by full path rather than through PATH: these are native
# Windows binaries and Git Bash does not accept "C:/..." entries in PATH.
GCC="$PPU_BIN/ppu-lv2-gcc.exe"
PRXSTRIP="$HOST_BIN/ppu-lv2-prx-strip.exe"
need() {
	for t in "$@"; do
		[ -f "$t" ] || { echo "faltando: $t"; exit 1; }
	done
}

# The kernel shim blob is built with LLVM (true LP64 big-endian PPC64), never
# the ILP32 ppu-lv2-gcc, which would truncate the kernel's 64-bit pointers.
# Only the VSH build embeds it; tools/shim_pack.py turns the linked ELF into
# src/shim_blob.h. Run from src/, so shim/ is ../shim and tools/ is ../tools.
build_shim_blob() {
	local CLANG="${CLANG:-clang}" LLD="${LLD:-ld.lld}" PY="${PY:-python}"
	command -v "$CLANG" >/dev/null 2>&1 || { echo "faltando: clang (shim blob)"; exit 1; }
	command -v "$LLD"   >/dev/null 2>&1 || { echo "faltando: ld.lld (shim blob)"; exit 1; }
	command -v "$PY"    >/dev/null 2>&1 || { echo "faltando: python (shim blob)"; exit 1; }
	local RES; RES="$("$CLANG" -print-resource-dir)/include"
	local CF=(--target=powerpc64-unknown-linux-gnu -std=c99 -O2 -ffreestanding
	          -fno-builtin -nostdinc -isystem "$RES" -fno-jump-tables
	          -fno-stack-protector -fno-pic -mcmodel=small -Wall -Wextra)
	echo "shim blob (LLVM)..."
	"$CLANG" "${CF[@]}" -c ../shim/entry.c      -o ../shim/entry.o
	"$CLANG" "${CF[@]}" -c ../shim/shim.c       -o ../shim/shim.o
	"$CLANG" "${CF[@]}" -c ../shim/ds3_report.c -o ../shim/ds3_report.o
	"$CLANG" "${CF[@]}" -c sixaxis.c            -o ../shim/sixaxis.o
	"$CLANG" "${CF[@]}" -c ds_body.c            -o ../shim/ds_body.o
	"$LLD" -m elf64ppc -T ../shim/shim.ld --emit-relocs --no-dynamic-linker -static \
	       ../shim/entry.o ../shim/shim.o ../shim/ds3_report.o ../shim/sixaxis.o ../shim/ds_body.o \
	       -o ../shim/shim.elf
	"$PY" -I ../tools/shim_pack.py ../shim/shim.elf shim_blob.h
}

need "$GCC" "$PRXSTRIP"
if [ -n "$TOOLS" ]; then
	# scetool resolves its key file through $PS3, so it works from any directory.
	export PS3="$TOOLS/data"
	SCETOOL="$TOOLS/scetool.exe"
	need "$SCETOOL" "$TOOLS/data/keys"
else
	MAKE_FSELF="$HOST_BIN/make_fself.exe"
	need "$MAKE_FSELF"
fi

mkdir -p obj
rm -f obj/*.o

# The VSH build embeds the kernel shim blob; regenerate src/shim_blob.h first.
if [ "$VARIANT" = "vsh" ]; then
	build_shim_blob
fi

echo "compilando..."
for f in $SRCS; do
	"$GCC" $CFLAGS -c "$f" -o "obj/${f%.c}.o"
done

echo "linkando..."
"$GCC" $LDFLAGS obj/*.o -L "$LIBDIR" $LDLIBS -o obj/ds45pad.sym

echo "strip..."
"$PRXSTRIP" --strip-debug --strip-section-header \
            obj/ds45pad.sym -o obj/ds45pad.prx

echo "assinando..."
if [ -n "$TOOLS" ]; then
	"$SCETOOL" $SCEFLAGS -e obj/ds45pad.prx "$OUT" > /dev/null
else
	# make_fself takes no auth id (loading on DEX is confirmed on console, Task 11).
	"$MAKE_FSELF" obj/ds45pad.prx "$OUT" > /dev/null
fi

# A SPRX that still starts with 7F 45 4C 46 ("ELF") means signing silently did
# nothing, and an unsigned module in boot_plugins.txt hangs HEN at boot.
magic=$(od -A n -t x1 -N 4 "$OUT" | tr -d ' ')
if [ "$magic" != "53434500" ]; then
	echo "ERRO: $OUT nao comeca com SCE (magic=$magic) - NAO instale"
	exit 1
fi

echo "ok: $OUT ($(stat -c %s "$OUT") bytes, magic SCE)"
