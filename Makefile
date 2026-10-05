# ---- Project ----
# WarpQuake: id's WinQuake (via erysdren/quakegeneric, C-only, no x86 asm) on
# an RTG screen.  quake/ is the engine, src/ the AmigaOS platform layer.
TARGET      := WarpQuake
BUILD_DIR   := build
# 'make' also copies the binary to TARGET_DIR (an emulator's shared drive, say)
# if that directory exists; 'make ftp' uploads to FTP_DIR on the Amiga.
TARGET_DIR  ?= ~/Documents/Amiga/amiga_uae_disk/
FTP_DIR     ?= WORK:Games/warpQuake

# ---- CPU ----
# FPU build only: the renderer is float throughout.
CPU         ?= 68060
MATHLIB     := $(if $(filter 68040,$(CPU)),-lm040,-lm060)
# -O1, not -O2: vbcc 0.9h miscompiles some loops at -O2 (a loop index stepped in
# the body together with a break).
# OPT=2 is an experiment, to be validated against timedemo results.
OPT         ?= 1
# ASM=1: the 68060 routines in quake/*.s replace their C versions (WQ_ASM in
# quakedef.h).  ASM=0 is the all-C reference, for -crc comparisons.
ASM         ?= 1
OBJ_DIR     := $(BUILD_DIR)/obj-$(CPU)-O$(OPT)$(if $(filter 0,$(ASM)),-C)
SUFFIX      := $(if $(filter 68040,$(CPU)),_040)$(if $(filter-out 1,$(OPT)),_O$(OPT))$(if $(filter 0,$(ASM)),_C)
BIN         := $(BUILD_DIR)/$(TARGET)$(SUFFIX)

# ---- SDK / Includes ----
# Override any of these on the command line or in the environment, e.g.
#   make SDK_DIR=/opt/amiga P96_SDK=~/sdk/Picasso96Develop/Include
SDK_DIR     ?= /opt/amiga_sdk
NDK_INC_C   ?= $(SDK_DIR)/NDK3.2R4/Include_H
VBCC        ?= $(SDK_DIR)/vbcc
INC_FLAGS   := -Iquake -Isrc -I$(NDK_INC_C)
# The Picasso96 developer headers (libraries/Picasso96.h; the Picasso96Develop
# archive), after the NDK: their proto/ and pragmas/ are for other compilers,
# vbcc has its own inline/ stubs.  The default is where they sit when this
# repo is checked out next to them.
P96_SDK     ?= ../p96_gfx_driver/Picasso96Develop/Include
P96_INC     := -I$(P96_SDK)
DEF_FLAGS   := $(addprefix -D,$(DEFINES)) -DWQ_ASM=$(ASM)
# AHI's developer headers (devices/ahi.h; the ahidev archive on Aminet), for
# src/amiga_ahi.c; vbcc has the proto/inline stubs.
AHI_SDK     ?= ../warpAHIDriver/ahidev_4.18/AHI/Developer/include/C
AHI_INC     := -I$(AHI_SDK)

# ---- Toolchain ----
CC      := vc
AS      := vasmm68k_mot
RM      := rm -rf
MKDIR   := mkdir -p

# ---- Flags ----
OPTFLAG := -O$(OPT)
CFLAGS  = +aos68k_std -cpu=$(CPU) -fpu=$(CPU) -c99 $(OPTFLAG) -dontwarn=153 -dontwarn=214 -dontwarn=208 -dontwarn=81
LIBS    := -lamiga -ldebug $(MATHLIB)
LDFLAGS := $(LIBS)
ASFLAGS := -quiet -Fhunk -m68060 -Iquake

# ---- Sources ----
# The engine list is quakegeneric's own (its CMakeLists.txt), less its null
# backend: src/ provides main() and the QG_* hooks.
ENGINE_SRC  := $(filter-out quake/quakegeneric_null.c,$(wildcard quake/*.c))
ENGINE_OBJS := $(patsubst quake/%.c,$(OBJ_DIR)/quake/%.o,$(ENGINE_SRC))
ENGINE_ASM  := $(if $(filter 1,$(ASM)),$(wildcard quake/*.s))
ENGINE_OBJS += $(patsubst quake/%.s,$(OBJ_DIR)/quake/%.o,$(ENGINE_ASM))
APP_SRC     := $(wildcard src/*.c)
APP_OBJS    := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(APP_SRC))
APP_OBJS    += $(patsubst src/%.s,$(OBJ_DIR)/src/%.o,$(wildcard src/*.s))
OBJS        := $(APP_OBJS) $(ENGINE_OBJS)

.PHONY: all app prof clean distclean install ftp ftp-data dist

# 'all' does not clean first; 'make -j8' is worth it.
all: app install

app: $(BIN)

$(BIN): $(OBJS)
	@echo '------  LINK $@ ------'
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/src/%.o: src/%.c $(wildcard src/*.h)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) $(INC_FLAGS) $(P96_INC) $(AHI_INC) $(DEF_FLAGS) -c $< -o $@

# Engine objects quietly, the log printed only on failure (as MuPDF's are).
$(OBJ_DIR)/quake/%.o: quake/%.c $(wildcard quake/*.h)
	@$(MKDIR) $(@D)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) $(INC_FLAGS) $(DEF_FLAGS) -c $< -o $@ > $@.log 2>&1 || { cat $@.log; exit 1; }

$(OBJ_DIR)/src/%.o: src/%.s
	@$(MKDIR) $(@D)
	$(AS) $(ASFLAGS) -o $@ $<

$(OBJ_DIR)/quake/%.o: quake/%.s
	@$(MKDIR) $(@D)
	@echo "  AS $<"
	@$(AS) $(ASFLAGS) -o $@ $< > $@.log 2>&1 || { cat $@.log; exit 1; }

# The 16 bpp builds of the asm drawers: quake/X060rgb.s sets PIX16 and
# includes quake/X060.s, so it is rebuilt when that changes.
$(foreach f,$(wildcard quake/*060rgb.s),$(eval $(OBJ_DIR)/quake/$(notdir $(f:.s=.o)): $(f:rgb.s=.s)))

# Engine files to build at -O2, the rest at -O1: an experiment knob, empty by
# default.  13 hot files (r_draw r_bsp d_polyse r_alias r_aclip d_edge r_misc
# r_light r_part d_part r_surf d_surf cl_main) gave identical frames at -O2
# but ran SLOWER on the 060 (world BSP 6.98 -> 7.34 ms, demo1, 2026-10-04).
# The whole engine at -O2 does not even give identical frames.
O2_FILES ?=
$(foreach f,$(O2_FILES),$(eval $(OBJ_DIR)/quake/$(f).o: OPTFLAG := -O2))

# For tools/wprof.py: the same objects linked without stripping, plus a map.
VBCC_LIB := $(VBCC)/targets/m68k-amigaos/lib
prof: $(OBJS)
	vlink -bamigahunk -Bstatic -Cvbcc -nostdlib -mrel $(VBCC_LIB)/startup.o $(OBJS) \
		-L$(VBCC_LIB) -lamiga -ldebug $(MATHLIB) -lvc -M$(BUILD_DIR)/$(TARGET)_sym.map -o $(BUILD_DIR)/$(TARGET)_sym

# A release: build/dist/WarpQuake-<version>.lha and .zip, each holding a
# WarpQuake drawer (program, icon, readme, LICENSE), plus the readme beside
# them (Aminet-style).  The version is the one in $VER (src/amiga_main.c).
# Needs lha (LHa for UNIX) and zip.
VERSION  := $(shell sed -n 's/.*VERSION_STRING "WarpQuake \([^ ]*\) .*/\1/p' src/amiga_main.c)
DIST_DIR := $(BUILD_DIR)/dist
dist: app
	@echo '------  DIST $(VERSION)  ------'
	$(RM) $(DIST_DIR)
	$(MKDIR) $(DIST_DIR)/WarpQuake
	cp $(BIN) $(DIST_DIR)/WarpQuake/WarpQuake
	cp dist/WarpQuake.info dist/WarpQuake.readme LICENSE $(DIST_DIR)/WarpQuake/
	cd $(DIST_DIR) && lha aq WarpQuake-$(VERSION).lha WarpQuake && zip -qr WarpQuake-$(VERSION).zip WarpQuake
	cp dist/WarpQuake.readme $(DIST_DIR)/WarpQuake-$(VERSION).readme
	@ls -l $(DIST_DIR)

clean:
	@echo '------  CLEAN  ------'
	$(RM) $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/$(TARGET)_* $(BUILD_DIR)/obj-*/src

distclean:
	$(RM) $(BUILD_DIR)

install: $(BIN)
	@if [ -d $(TARGET_DIR) ]; then echo '------ INSTALL ------'; cp $(BIN) $(TARGET_DIR); fi

# Uploads: tools/amiput.py, with the Amiga's FTP login from the AMIGA_FTP_*
# environment variables or tools/ftp_config.local (git-ignored; see
# tools/ftp_config.local.example).
FTP_PUT ?= python3 tools/amiput.py

ftp: app
	$(FTP_PUT) $(BIN) -d $(FTP_DIR)

# One-off: the game data (about 54 MB for the registered paks) from QUAKE_DATA
# into $(FTP_DIR)/id1, which must exist.
QUAKE_DATA ?= id1
ftp-data:
	for f in $(wildcard $(QUAKE_DATA)/*.pak $(QUAKE_DATA)/*.PAK $(QUAKE_DATA)/*.Pak); do \
		$(FTP_PUT) "$$f" -d $(FTP_DIR)/id1 || exit 1; done
