# ---- Project ----
# WarpQuake: id's WinQuake (via erysdren/quakegeneric, C-only, no x86 asm) on
# an RTG screen.  quake/ is the engine, src/ the AmigaOS platform layer.
TARGET      := WarpQuake
BUILD_DIR   := build
TARGET_DIR  ?= ~/Documents/Amiga/amiga_uae_disk/
FTP_DIR     ?= WORK:Games/warpQuake

# ---- CPU ----
# FPU build only: the renderer is float throughout.
CPU         ?= 68060
MATHLIB     := $(if $(filter 68040,$(CPU)),-lm040,-lm060)
# -O1, not -O2: vbcc 0.9h has a known -O2 loop miscompile (see warpPDFViewer).
# OPT=2 is an experiment, to be validated against timedemo results.
OPT         ?= 1
OBJ_DIR     := $(BUILD_DIR)/obj-$(CPU)-O$(OPT)
SUFFIX      := $(if $(filter 68040,$(CPU)),_040)$(if $(filter-out 1,$(OPT)),_O$(OPT))
BIN         := $(BUILD_DIR)/$(TARGET)$(SUFFIX)

# ---- SDK / Includes ----
SDK_DIR     := /opt/amiga_sdk
NDK_INC_C   := $(SDK_DIR)/NDK3.2R4/Include_H
VBCC        ?= $(SDK_DIR)/vbcc
INC_FLAGS   := -Iquake -Isrc -I$(NDK_INC_C)
# Picasso96 structures from the repo's P96 SDK, after the NDK (its proto/ and
# pragmas/ are for other compilers; vbcc has its own inline/ stubs).
P96_INC     := -I../p96_gfx_driver/Picasso96Develop/Include
DEF_FLAGS   := $(addprefix -D,$(DEFINES))

# ---- Toolchain ----
CC      := vc
RM      := rm -rf
MKDIR   := mkdir -p

# ---- Flags ----
OPTFLAG := -O$(OPT)
CFLAGS  = +aos68k_std -cpu=$(CPU) -fpu=$(CPU) -c99 $(OPTFLAG) -dontwarn=153 -dontwarn=214 -dontwarn=208 -dontwarn=81
LIBS    := -lamiga -ldebug $(MATHLIB)
LDFLAGS := $(LIBS)

# ---- Sources ----
# The engine list is quakegeneric's own (its CMakeLists.txt), less its null
# backend: src/ provides main() and the QG_* hooks.
ENGINE_SRC  := $(filter-out quake/quakegeneric_null.c,$(wildcard quake/*.c))
ENGINE_OBJS := $(patsubst quake/%.c,$(OBJ_DIR)/quake/%.o,$(ENGINE_SRC))
APP_SRC     := $(wildcard src/*.c)
APP_OBJS    := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(APP_SRC))
OBJS        := $(APP_OBJS) $(ENGINE_OBJS)

.PHONY: all app prof clean distclean install ftp ftp-data

# Like warpPDFViewer, 'all' does not clean first; 'make -j8' is worth it.
all: app install

app: $(BIN)

$(BIN): $(OBJS)
	@echo '------  LINK $@ ------'
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(OBJ_DIR)/src/%.o: src/%.c $(wildcard src/*.h)
	@$(MKDIR) $(@D)
	$(CC) $(CFLAGS) $(INC_FLAGS) $(P96_INC) $(DEF_FLAGS) -c $< -o $@

# Engine objects quietly, the log printed only on failure (as MuPDF's are).
$(OBJ_DIR)/quake/%.o: quake/%.c $(wildcard quake/*.h)
	@$(MKDIR) $(@D)
	@echo "  CC $<"
	@$(CC) $(CFLAGS) $(INC_FLAGS) $(DEF_FLAGS) -c $< -o $@ > $@.log 2>&1 || { cat $@.log; exit 1; }

# For tools/wprof.py: the same objects linked without stripping, plus a map.
VBCC_LIB := $(VBCC)/targets/m68k-amigaos/lib
prof: $(OBJS)
	vlink -bamigahunk -Bstatic -Cvbcc -nostdlib -mrel $(VBCC_LIB)/startup.o $(OBJS) \
		-L$(VBCC_LIB) -lamiga -ldebug $(MATHLIB) -lvc -M$(BUILD_DIR)/$(TARGET)_sym.map -o $(BUILD_DIR)/$(TARGET)_sym

clean:
	@echo '------  CLEAN  ------'
	$(RM) $(BUILD_DIR)/$(TARGET) $(BUILD_DIR)/$(TARGET)_* $(BUILD_DIR)/obj-*/src

distclean:
	$(RM) $(BUILD_DIR)

install: $(BIN)
	@echo '------ INSTALL ------'
	cp $(BIN) $(TARGET_DIR)

# ftpCopy.py reads AMIGA_FTP_* from the environment; tools/ftp_config.local
# (git-ignored, the file tools/amiget.py reads) supplies them if present.
FTP_ENV := set -a; [ -f tools/ftp_config.local ] && . tools/ftp_config.local; set +a;

ftp: app
	$(FTP_ENV) python3 ../utils/PyFtpCopy/ftpCopy.py -s $(BIN) -d $(FTP_DIR)

# One-off: the game data (about 54 MB for the registered paks).  ftpCopy.py
# copies single files into an existing directory: create $(FTP_DIR)/id1 first.
QUAKE_DATA ?= ../../Quake_TyrQuake/Id1
ftp-data:
	for f in $(wildcard $(QUAKE_DATA)/*.pak $(QUAKE_DATA)/*.PAK $(QUAKE_DATA)/*.Pak); do \
		$(FTP_ENV) python3 ../utils/PyFtpCopy/ftpCopy.py -s "$$f" -d $(FTP_DIR)/id1 || exit 1; done
