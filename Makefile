# VS. Super Mario Bros. native port - build
#
#   make headless        headless test harness (interpreter only)
#   make recomp          run the recompiler on rom/*.nes -> generated/
#   make vssmb           native game executable (needs generated/ and SDL2)
#   make test            CPU/PPU/APU/mapper regression tests
#
# Requires: a C11 compiler, GNU make, Python 3; SDL2 for the frontend.

ifeq ($(origin CC),default)
  CC := gcc
endif
PYTHON  ?= python
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
LDFLAGS ?=
# Windows detection: $(OS) is set by cmd/PowerShell, uname by MSYS2/MinGW shells
UNAME := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring Windows_NT,$(OS))$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
  WINDOWS := 1
  EXE := .exe
else
  WINDOWS := 0
  EXE :=
endif

# first .nes (or MAME's suprmrio.zip) in rom/ (filenames may contain spaces, so avoid $(firstword))
ROM     ?= $(shell ls rom/*.nes rom/*.zip 2>/dev/null | head -n 1)
BUILD   ?= build
GEN     := generated

RUNTIME_SRC := runtime/cpu.c runtime/mem.c runtime/cart.c runtime/mapper.c runtime/ppu.c \
               runtime/apu.c runtime/sys.c runtime/png.c runtime/palette.c runtime/romzip.c runtime/widescreen.c
RUNTIME_OBJ := $(patsubst runtime/%.c,$(BUILD)/%.o,$(RUNTIME_SRC))

GEN_SRC := $(wildcard $(GEN)/*.c)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(BUILD)/gen/%.o,$(GEN_SRC))

SDL_CONFIG ?= sdl2-config
SDL_CFLAGS := $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
ifeq ($(WINDOWS),1)
  # single-file executable: SDL2 and the MinGW runtime (libgcc, winpthread) linked statically, so
  # nothing but Windows' own DLLs is needed; a stray 32-bit libwinpthread-1.dll on the PATH would
  # otherwise stop the program with error 0xc000007b
  SDL_LIBS := -static $(shell $(SDL_CONFIG) --static-libs 2>/dev/null) -lcomdlg32 -ldxgi -static-libgcc
  PLATFORM := win64
  ARCHIVE := zip
else
  ifeq ($(SDL_STATIC),1)
    # SDL2 linked in (it loads its display, sound and input backends at run time), so the
    # binary runs without an SDL installation: see tools/linux/build.sh
    SDL_LIBS := $(shell $(SDL_CONFIG) --static-libs 2>/dev/null) -static-libgcc
  else
    SDL_LIBS := $(shell $(SDL_CONFIG) --libs 2>/dev/null)
  endif
  PLATFORM := $(shell uname -s | tr A-Z a-z)-$(shell uname -m)
  ARCHIVE := gztar
endif
# the Steam Deck edition: Deck defaults (fullscreen 1280x800, 16:10, pad-first) on every machine
ifeq ($(DECK_EDITION),1)
  CFLAGS += -DDECK_EDITION
  PLATFORM := steamdeck
endif

FRONTEND_SRC := frontend/sdl_main.c frontend/font.c frontend/ui.c frontend/filedialog.c frontend/audio_out.c frontend/input.c frontend/gpu_sync.c frontend/icon_data.c frontend/rewind.c frontend/touchpad.c
FRONTEND_OBJ := $(patsubst frontend/%.c,$(BUILD)/fe_%.o,$(FRONTEND_SRC))
ifeq ($(WINDOWS),1)
  # executable icon + version info (Explorer, taskbar)
  WINDRES ?= windres
  FRONTEND_OBJ += $(BUILD)/fe_resources.o
endif

.PHONY: all headless recomp vssmb test cosim dist clean

all: headless

$(BUILD)/%.o: runtime/%.c runtime/nes.h runtime/palette.h runtime/romzip.h runtime/widescreen.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

GEN_CFLAGS := -Wno-unused-label -Wno-implicit-fallthrough -Wno-unused-variable

$(BUILD)/gen/%.o: $(GEN)/%.c runtime/nes.h runtime/recomp_rt.h
	@mkdir -p $(BUILD)/gen
	$(CC) $(CFLAGS) $(GEN_CFLAGS) $(RECOMP_DEFS) -Iruntime -c $< -o $@

# ---- headless harness (interpreter only) ----
headless: $(BUILD)/nes-headless$(EXE)

$(BUILD)/nes-headless$(EXE): $(RUNTIME_OBJ) $(BUILD)/headless.o $(BUILD)/recomp_stub.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# ---- headless harness with recompiled code ----
headless-recomp: $(BUILD)/nes-headless-recomp$(EXE)

$(BUILD)/nes-headless-recomp$(EXE): $(RUNTIME_OBJ) $(BUILD)/headless.o $(GEN_OBJ)
	@test -n "$(GEN_SRC)" || (echo "no generated code: run 'make recomp' first" && exit 1)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lm

# ---- recompiler ----
recomp:
	@test -n "$(ROM)" || (echo "put your VS. Super Mario Bros. .nes file or MAME's suprmrio.zip in rom/ (or pass ROM=path)" && exit 1)
	$(PYTHON) recomp/recomp.py --rom "$(ROM)" --out $(GEN) $(RECOMP_FLAGS)

# ---- native game ----
vssmb: $(BUILD)/vssmb$(EXE)

$(BUILD)/vssmb$(EXE): $(RUNTIME_OBJ) $(FRONTEND_OBJ) $(GEN_OBJ)
	@test -n "$(GEN_SRC)" || (echo "no generated code: run 'make recomp' first" && exit 1)
	$(CC) $(CFLAGS) $^ -o $@ $(SDL_LIBS) $(LDFLAGS) -lm

$(BUILD)/fe_%.o: frontend/%.c runtime/nes.h runtime/palette.h runtime/widescreen.h frontend/font.h frontend/ui.h frontend/filedialog.h frontend/audio_out.h frontend/input.h frontend/gpu_sync.h frontend/rewind.h frontend/touchpad.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Iruntime -c $< -o $@

$(BUILD)/fe_resources.o: frontend/vssmb.rc frontend/vssmb.ico
	@mkdir -p $(BUILD)
	$(WINDRES) -I frontend $< -O coff -o $@

# ---- distributable package: the executable plus the player notes ----
dist: $(BUILD)/vssmb$(EXE)
	rm -rf dist/vssmb && mkdir -p dist/vssmb
	cp $(BUILD)/vssmb$(EXE) dist/vssmb/
	cp docs/PLAYERS.md dist/vssmb/README.txt
	@test "$(WINDOWS)" = 1 || cp frontend/icon_preview.png dist/vssmb/vssmb.png
	$(PYTHON) -c "import shutil; shutil.make_archive('dist/vssmb-$(PLATFORM)', '$(ARCHIVE)', 'dist', 'vssmb')"
	@echo "package: dist/vssmb-$(PLATFORM).$(if $(filter zip,$(ARCHIVE)),zip,tar.gz)"

# ---- tests ----
test: headless
	$(PYTHON) tests/run_tests.py

cosim: headless headless-recomp
	$(PYTHON) tests/cosim.py

clean:
	rm -rf $(BUILD)
