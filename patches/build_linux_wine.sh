#!/bin/bash
# Usage: npp_build.sh <source_folder> <vsh|game> <output_file.sprx>
# Replicates NativePadPS3 build.sh on Linux: Cell SDK compiler under Wine + native scetool.
#
# Override these for your environment (defaults shown in brackets):
#   PS3SDK      Cell SDK root, containing host-win32/ and target/ppu/   [/opt/cell]
#   SCEDIR      directory holding the `scetool` binary and its `data/`  [$HOME/scetool]
#   WINEPREFIX  Wine prefix set up with the SDK's win32 toolchain       [$HOME/.wine-ps3]
#   WINE        Wine executable                                         [wine, from PATH]
set -e
SRCDIR="$1"; MODE="$2"; OUT="$3"
SDK="${PS3SDK:-/opt/cell}"
SCEDIR="${SCEDIR:-$HOME/scetool}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ps3}"
export WINEARCH=win32 WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" DISPLAY=
WINE="${WINE:-wine}"
GCC="$SDK/host-win32/ppu/bin/ppu-lv2-gcc.exe"
PRXSTRIP="$SDK/host-win32/bin/ppu-lv2-prx-strip.exe"
INCLUDE="$SDK/target/ppu/include"; LIBDIR="$SDK/target/ppu/lib"

AUTH_VSH=1070000052000001; AUTH_GAME=1010000001000003
if [ "$MODE" = "game" ]; then
  EXTRA="-DNPP_GAME"; AUTH=$AUTH_GAME
  SRCS="libc.c util.c hook.c config.c sticks.c diag.c main.c"; LDLIBS="-lio_stub -lfs_stub"
else
  EXTRA=""; AUTH=$AUTH_VSH
  SRCS="libc.c util.c hook.c config.c ds4.c dualsense.c sticks.c padldd.c usb.c passthru.c diag.c main.c"
  LDLIBS="-lusbd_stub -lio_stub -lfs_stub -llv2_stub"
fi
CFLAGS="-include compat_pad.h -mprx -Os -ffunction-sections -fdata-sections -fno-builtin-printf -nodefaultlibs -std=gnu99 -Wall -Wno-unused-parameter -I $INCLUDE $EXTRA"
LDFLAGS="-mprx -zgenprx -zgenstub -ffunction-sections -fdata-sections"
SCEFLAGS="-0 SELF -1 TRUE -s FALSE -2 0A -3 $AUTH -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000"

cd "$SRCDIR"; mkdir -p obj; rm -f obj/*.o
echo "compiling ($MODE)..."
for f in $SRCS; do $WINE "$GCC" $CFLAGS -c "$f" -o "obj/${f%.c}.o"; done
echo "linking..."
$WINE "$GCC" $LDFLAGS obj/*.o -L "$LIBDIR" $LDLIBS -o obj/nativepadps3.sym
echo "strip..."
$WINE "$PRXSTRIP" --strip-debug --strip-section-header obj/nativepadps3.sym -o obj/nativepadps3.prx
echo "signing..."
PS3="$SCEDIR/data" "$SCEDIR/scetool" $SCEFLAGS -e obj/nativepadps3.prx "$OUT" > /dev/null
magic=$(od -A n -t x1 -N 4 "$OUT" | tr -d ' ')
[ "$magic" = "53434500" ] || { echo "ERROR: $OUT does not start with SCE ($magic)"; exit 1; }
echo "ok: $OUT ($(stat -c %s "$OUT") bytes)"
