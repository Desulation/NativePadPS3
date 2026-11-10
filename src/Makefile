# NPP - PS3 VSH plugin build
#
# Requires the Cell SDK PPU toolchain (ppu-lv2-gcc) and scetool on PATH, the
# same combination webMAN MOD and PS3xPAD are built with. See ../docs/BUILD.md.
#
#   make            -> nativepadps3_vsh.sprx  (plugin loader / XMB build)
#   make game       -> nativepadps3_game.sprx (optional in-game build)
#   make all-builds -> both
#   make clean
#   make DEBUG=1 ... -> same targets with -DNPP_DEBUG (boot trace, LV2 dumper),
#                       output named nativepadps3_<variant>_debug.sprx

CELL_MK_DIR = $(CELL_SDK)/samples/mk
include $(CELL_MK_DIR)/sdk.makedef.mk

CELL_BUILD_TOOLS = GCC

PPU_AR              = ppu-lv2-ar
PPU_RANLIB          = ppu-lv2-ranlib
PPU_STRIP           = ppu-lv2-strip
PPU_CC              = ppu-lv2-gcc
PPU_CXX             = ppu-lv2-g++
PPU_LD              = ppu-lv2-ld
PPU_CCLD            = ppu-lv2-gcc
PPU_CXXLD           = ppu-lv2-g++
PPU_PRX_CCLD        = ppu-lv2-gcc
PPU_PRX_CXXLD       = ppu-lv2-g++
PPU_PRX_STRIP       = ppu-lv2-prx-strip

PPU_PRX_FLAGS = -mprx -mno-sn-ld -Os -ffunction-sections -fdata-sections \
                -fno-builtin-printf -nodefaultlibs -std=gnu99 \
                -Wall -Wno-shadow -Wno-unused-parameter

# The shim rebuilds cellPadGetInfo2/cellPadSetPortSetting for old SDKs (192.001)
# that lack them; an SDK that declares them (475.001) must not get it.
# Forward slashes: $(shell) runs sh, which would eat the backslashes of a
# Windows-style CELL_SDK such as C:\usr\local\cell.
COMPAT_INC := $(shell grep -q cellPadGetInfo2 "$(subst \,/,$(CELL_SDK))/target/ppu/include/cell/pad/libpad.h" 2>/dev/null || echo -include ../patches/compat_pad.h)
EXTRA_CFLAGS    ?= $(COMPAT_INC)
PPU_CFLAGS      = $(PPU_PRX_FLAGS) $(EXTRA_CFLAGS)
PPU_LDFLAGS     += $(PPU_PRX_FLAGS)
PPU_PRX_LDFLAGS += -mprx -ffunction-sections -fdata-sections -Wl,--strip-unused-data

PPU_PRX_STRIPFLAGS += --strip-debug --strip-section-header

# DEBUG=1 defines NPP_DEBUG for either variant and tags the output name.
# DEBUG is a make command-line variable, so it reaches the recursive makes.
ifeq ($(DEBUG),1)
PPU_PRX_FLAGS += -DNPP_DEBUG
DEBUG_SUFFIX = _debug
SRCS_VSH_DEBUG = lv2dump.c lv2capture.c
else
DEBUG_SUFFIX =
SRCS_VSH_DEBUG =
endif

# The two builds do NOT share a source list or a library list.
#
# A game process loads libio and libfs but *not* libusbd. A single reference to
# libusbd leaves the loader unable to resolve the module, and PS3MAPI then sits
# on "Load" forever - so usb.c, passthru.c and the pad-LDD code, which are all
# VSH-only anyway, must be absent from the in-game build rather than merely
# unused by it.
SRCS_VSH  = libc.c util.c hook.c config.c paths.c ds4.c dualsense.c ds_body.c sixaxis.c \
            sticks.c padldd.c usb.c passthru.c lv2.c kernel.c btcheck.c \
            kpatch.c kshim.c kshim_table.c diag.c \
            main.c $(SRCS_VSH_DEBUG)
SRCS_GAME = libc.c util.c hook.c config.c paths.c sticks.c gamehook.c \
            diag.c main.c

LIBS_VSH  = -lusbd_stub -lio_stub -lfs_stub -llv2_stub
LIBS_GAME = -lio_stub -lfs_stub -llv2_stub

# Default to the VSH build so a bare `make` in this directory still works.
PPU_SRCS       = $(SRCS_VSH)
PPU_PRX_LDLIBS = $(LIBS_VSH)

PPU_PRX_TARGET = nativepadps3.prx

# Signing parameters.
#
# -2 0A is the 3.55 key revision and it is not optional for PS3HEN: a plugin
# signed with an older revision makes HEN hang on boot. This is why PS3xPAD,
# which signs with -2 04, had to be re-released as "3.55 signed for HEN".
# webMAN MOD uses the values below and loads on both CFW and HEN.
#
# The auth id is passed per target, not baked in here: it decides where the
# module is allowed to live, and the two builds live in different places.
#   1070000052000001 = sys/internal + vsh/module   -> the VSH build
#   1010000001000003 = retail game/update          -> the in-game build
# Signing the in-game build with the VSH id produces a module a game process
# will not load.
SCETOOL_FLAGS = -0 SELF -1 TRUE -s FALSE -2 0A \
                -4 01000002 -5 APP -6 0003004000000000 -A 0001000000000000

AUTH_VSH  = 1070000052000001
AUTH_GAME = 1010000001000003

# SIGN=scetool (default) signs with retail keys for CEX CFW and HEN.
# SIGN=fself uses the SDK's make_fself instead: a fake-signed SELF that only
# DEX/debug firmware loads, and it takes no auth id.
SIGN ?= scetool

SPRX_VSH  = nativepadps3_vsh$(DEBUG_SUFFIX).sprx
SPRX_GAME = nativepadps3_game$(DEBUG_SUFFIX).sprx

all: vsh

# Both targets clean first. Without that, objects left over from the other
# variant get linked in silently - a "VSH" build made after a `make game` would
# contain -DNPP_GAME code.
vsh: shim-blob
	$(MAKE) clean-obj
	$(MAKE) sign-one PPU_SRCS="$(SRCS_VSH)" PPU_PRX_LDLIBS="$(LIBS_VSH)" \
	                 AUTH=$(AUTH_VSH) SPRX=$(SPRX_VSH)

game:
	$(MAKE) clean-obj
	$(MAKE) sign-one PPU_SRCS="$(SRCS_GAME)" PPU_PRX_LDLIBS="$(LIBS_GAME)" \
	                 EXTRA_CFLAGS="-DNPP_GAME $(COMPAT_INC)" \
	                 AUTH=$(AUTH_GAME) SPRX=$(SPRX_GAME)

sign-one:
	$(MAKE) $(PPU_OBJS_DEPENDS)
	$(PPU_PRX_STRIP) $(PPU_PRX_STRIPFLAGS) $(PPU_PRX_TARGET)
ifeq ($(SIGN),fself)
	$(CELL_SDK)/host-win32/bin/make_fself $(PPU_PRX_TARGET) $(SPRX)
else
	scetool $(SCETOOL_FLAGS) -3 $(AUTH) -e $(PPU_PRX_TARGET) $(SPRX)
endif
	@# A SPRX still starting with 7F 45 4C 46 ("ELF") means signing silently
	@# did nothing, and an unsigned module in boot_plugins.txt hangs HEN at
	@# boot. Delete it rather than leave it lying around to be installed.
	@magic=`od -A n -t x1 -N 4 $(SPRX) | tr -d ' '`; \
	 if [ "$$magic" != "53434500" ]; then \
	   echo "ERROR: $(SPRX) does not start with SCE (magic=$$magic) - do NOT install"; \
	   rm -f $(SPRX); exit 1; \
	 fi
	@echo "ok: $(SPRX) assinado (magic SCE)"

all-builds:
	$(MAKE) vsh
	$(MAKE) game

# The kernel shim blob the VSH plugin embeds. Built with LLVM (true LP64
# big-endian PPC64), never the ILP32 ppu-lv2-gcc which would truncate the
# kernel's 64-bit pointers; tools/shim_pack.py packs it into src/shim_blob.h.
# Forward slashes: the resource dir may contain spaces (Program Files), so it
# is quoted; $(subst) drops the Windows backslashes $(shell) returns.
SHIM_CLANG  ?= clang
SHIM_LLD    ?= ld.lld
SHIM_PY     ?= python
SHIM_RESINC := $(subst \,/,$(shell $(SHIM_CLANG) -print-resource-dir))/include
SHIM_CFLAGS  = --target=powerpc64-unknown-linux-gnu -std=c99 -O2 -ffreestanding \
               -fno-builtin -nostdinc -isystem "$(SHIM_RESINC)" -fno-jump-tables \
               -fno-stack-protector -fno-pic -mcmodel=small -Wall -Wextra

shim-blob:
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c ../shim/entry.c      -o ../shim/entry.o
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c ../shim/shim.c       -o ../shim/shim.o
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c ../shim/ds3_report.c -o ../shim/ds3_report.o
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c ../shim/ds3_output.c -o ../shim/ds3_output.o
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c sixaxis.c            -o ../shim/sixaxis.o
	$(SHIM_CLANG) $(SHIM_CFLAGS) -c ds_body.c            -o ../shim/ds_body.o
	$(SHIM_LLD) -m elf64ppc -T ../shim/shim.ld --emit-relocs --no-dynamic-linker \
	    -static ../shim/entry.o ../shim/shim.o ../shim/ds3_report.o ../shim/ds3_output.o ../shim/sixaxis.o \
	    ../shim/ds_body.o -o ../shim/shim.elf
	$(SHIM_PY) -I ../tools/shim_pack.py ../shim/shim.elf shim_blob.h

clean-obj:
	-rm -f *.o $(PPU_PRX_TARGET)

CLEANFILES = nativepadps3_vsh.sprx nativepadps3_game.sprx nativepadps3_vsh_debug.sprx \
             nativepadps3_game_debug.sprx $(PPU_PRX_TARGET)

include $(CELL_MK_DIR)/sdk.target.mk
