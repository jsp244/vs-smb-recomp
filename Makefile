# VS. Super Mario Bros. native port - build
#
#   make headless        headless test harness (interpreter only)
#   make recomp          run the recompiler on rom/*.nes -> generated/
#   make vssmb           native game executable (needs generated/ and SDL2)
#   make app             macOS: the game as an app (VS. Super Mario Bros.app)
#   make test            CPU/PPU/APU/mapper regression tests
#
# Requires: a C11 compiler, GNU make, Python 3; SDL2 for the frontend.

# Windows detection: $(OS) is set by cmd/PowerShell, uname by MSYS2/MinGW shells
UNAME := $(shell uname -s 2>/dev/null)
ifeq ($(origin CC),default)
  CC := $(if $(filter Darwin,$(UNAME)),clang,gcc)
endif
PYTHON  ?= python
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
LDFLAGS ?=
ifneq (,$(findstring Windows_NT,$(OS))$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME)))
  WINDOWS := 1
  EXE := .exe
else
  WINDOWS := 0
  EXE :=
endif
MACOS := $(if $(filter Darwin,$(UNAME)),1,0)

# first .nes (or MAME's suprmrio.zip) in rom/ (filenames may contain spaces, so avoid $(firstword))
ROM     ?= $(shell ls rom/*.nes rom/*.zip 2>/dev/null | head -n 1)
BUILD   ?= build
GEN     := generated

RUNTIME_SRC := runtime/cpu.c runtime/mem.c runtime/cart.c runtime/mapper.c runtime/ppu.c \
               runtime/apu.c runtime/sys.c runtime/png.c runtime/palette.c runtime/romzip.c runtime/widescreen.c \
               runtime/bandwalk.c runtime/cpu816.c runtime/snesppu.c runtime/spc700.c runtime/allstars.c
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
else ifeq ($(MACOS),1)
  # one program for Apple silicon and Intel Macs (MAC_ARCHS) on macOS 11 or newer, with SDL2 linked in:
  # tools/macos/build.sh builds SDL2 that way, for the same Macs. `make app` / `make dist` put it in an app.
  MAC_ARCHS ?= -arch arm64 -arch x86_64
  MAC_MIN ?= 11.0
  CFLAGS += $(MAC_ARCHS) -mmacosx-version-min=$(MAC_MIN)
  LDFLAGS += $(MAC_ARCHS) -mmacosx-version-min=$(MAC_MIN)
  SDL_LIBS := $(shell $(SDL_CONFIG) --static-libs 2>/dev/null)
  PLATFORM := macos
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

FRONTEND_SRC := frontend/sdl_main.c frontend/font.c frontend/ui.c frontend/filedialog.c frontend/audio_out.c frontend/input.c frontend/gpu_sync.c frontend/icon_data.c frontend/rewind.c frontend/touchpad.c \
                frontend/http.c frontend/updater.c
FRONTEND_OBJ := $(patsubst frontend/%.c,$(BUILD)/fe_%.o,$(FRONTEND_SRC))

# ---- HTTPS for the Updates tab (frontend/http.c): WinHTTP on Windows; elsewhere libcurl, whose
#      headers are needed to build while the library itself is loaded at run time, so the program
#      also starts on systems without it (the Updates tab then says so)
ifeq ($(WINDOWS),1)
  HTTP_LIBS := -lwinhttp
else ifeq ($(MACOS),1)
  # macOS has libcurl itself, and its headers in the SDK
  HTTP_CFLAGS := -DHTTP_CURL
  HTTP_LIBS := -lcurl
else ifneq ($(shell pkg-config --exists libcurl 2>/dev/null && echo yes),)
  HTTP_CFLAGS := -DHTTP_CURL_DLOPEN $(shell pkg-config --cflags libcurl)
  HTTP_LIBS := -ldl -lpthread
endif

# ---- the Updates tab (frontend/updater.c): this build's version (git describe: the newest v* tag,
#      plus the commits after it), the GitHub repository whose releases it checks (from the git
#      remote) and its package there (the name `make dist` gives it). Builds without git, such as
#      the container of tools/linux/build.sh, take VSSMB_VERSION and VSSMB_REPO from the
#      environment. A stamp file rebuilds updater.c when they change.
ifeq ($(origin VSSMB_VERSION),undefined)
  VSSMB_VERSION := $(or $(shell git describe --tags --match "v[0-9]*" --dirty 2>/dev/null),dev)
endif
ifeq ($(origin VSSMB_REPO),undefined)
  VSSMB_REPO := $(shell git remote get-url origin 2>/dev/null | sed -n -E 's|^.*github\.com[:/]+([^/]+/[^/]+)$$|\1|p' | sed 's|\.git$$||')
endif
UPDATE_DEFS := -DVSSMB_VERSION=\"$(VSSMB_VERSION)\" -DVSSMB_REPO=\"$(VSSMB_REPO)\" \
               -DVSSMB_PACKAGE=\"vssmb-$(PLATFORM).$(if $(filter zip,$(ARCHIVE)),zip,tar.gz)\"
ifneq ($(UPDATE_DEFS),$(file <$(BUILD)/updater-defs.txt))
  $(shell mkdir -p $(BUILD))
  $(file >$(BUILD)/updater-defs.txt,$(UPDATE_DEFS))
endif
ifeq ($(WINDOWS),1)
  # executable icon + version info (Explorer, taskbar)
  WINDRES ?= windres
  FRONTEND_OBJ += $(BUILD)/fe_resources.o
endif
ifeq ($(MACOS),1)
  # the open-file panel and the Trash (Cocoa)
  FRONTEND_OBJ += $(BUILD)/fe_macos.o
endif
# the app's version for macOS (Info.plist): the numbers of VSSMB_VERSION
MAC_VERSION = $(or $(shell echo '$(VSSMB_VERSION)' | sed -n -E 's/^v?([0-9]+(\.[0-9]+){0,2}).*/\1/p'),0.0.0)
MAC_APP_DIR = $(BUILD)/app/VS. Super Mario Bros.app

.PHONY: all headless recomp vssmb app test cosim dist clean

all: headless

$(BUILD)/%.o: runtime/%.c runtime/nes.h runtime/palette.h runtime/romzip.h runtime/widescreen.h runtime/allstars.h runtime/cpu816.h runtime/snesppu.h runtime/spc700.h
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
	$(CC) $(CFLAGS) $^ -o $@ $(SDL_LIBS) $(HTTP_LIBS) $(LDFLAGS) -lm

$(BUILD)/fe_%.o: frontend/%.c runtime/nes.h runtime/palette.h runtime/widescreen.h runtime/romzip.h frontend/font.h frontend/ui.h frontend/filedialog.h frontend/audio_out.h frontend/input.h frontend/gpu_sync.h frontend/rewind.h frontend/touchpad.h frontend/http.h frontend/updater.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(HTTP_CFLAGS) -Iruntime -c $< -o $@

$(BUILD)/fe_updater.o: CFLAGS += $(UPDATE_DEFS)
$(BUILD)/fe_updater.o: $(BUILD)/updater-defs.txt

$(BUILD)/fe_resources.o: frontend/vssmb.rc frontend/vssmb.ico
	@mkdir -p $(BUILD)
	$(WINDRES) -I frontend $< -O coff -o $@

$(BUILD)/fe_macos.o: frontend/macos.m frontend/macos.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fobjc-arc $(SDL_CFLAGS) -c $< -o $@

# ---- macOS: the app (Info.plist from tools/macos/Info.plist with this version, the icon), signed ad hoc
#      as Apple silicon requires; nothing is written into it later (settings and saves go to Application
#      Support, and an update replaces the whole app) ----
app: $(BUILD)/vssmb$(EXE)
	rm -rf "$(BUILD)/app" && mkdir -p "$(MAC_APP_DIR)/Contents/MacOS" "$(MAC_APP_DIR)/Contents/Resources"
	cp $(BUILD)/vssmb "$(MAC_APP_DIR)/Contents/MacOS/vssmb"
	cp frontend/vssmb.icns "$(MAC_APP_DIR)/Contents/Resources/vssmb.icns"
	cp docs/PLAYERS.md "$(MAC_APP_DIR)/Contents/Resources/README.txt"
	sed 's/@VERSION@/$(MAC_VERSION)/g' tools/macos/Info.plist > "$(MAC_APP_DIR)/Contents/Info.plist"
	printf 'APPL????' > "$(MAC_APP_DIR)/Contents/PkgInfo"
	codesign --force --sign - --timestamp=none "$(MAC_APP_DIR)"
	@echo "app: $(MAC_APP_DIR) (version $(MAC_VERSION))"

# ---- distributable package: the executable (on macOS the app) plus the player notes (the in-app
#      update writes the notes too: frontend/updater.c's doc_names), and in the Deck edition
#      add-to-steam.py, made executable here (git stores it as 100644, and cp keeps that) ----
dist: $(if $(filter 1,$(MACOS)),app,$(BUILD)/vssmb$(EXE))
	rm -rf dist/vssmb && mkdir -p dist/vssmb
	@if [ "$(MACOS)" = 1 ]; then cp -R "$(MAC_APP_DIR)" dist/vssmb/; else cp $(BUILD)/vssmb$(EXE) dist/vssmb/; fi
	cp docs/PLAYERS.md dist/vssmb/README.txt
	@test "$(DECK_EDITION)" != 1 || cp docs/STEAMDECK.md dist/vssmb/STEAMDECK.txt
	@test "$(DECK_EDITION)" != 1 || { cp tools/steamdeck/add-to-steam.py dist/vssmb/ && chmod 755 dist/vssmb/add-to-steam.py; }
	@test "$(WINDOWS)" = 1 || test "$(MACOS)" = 1 || cp frontend/icon_preview.png dist/vssmb/vssmb.png
	$(PYTHON) -c "import shutil; shutil.make_archive('dist/vssmb-$(PLATFORM)', '$(ARCHIVE)', 'dist', 'vssmb')"
	@echo "package: dist/vssmb-$(PLATFORM).$(if $(filter zip,$(ARCHIVE)),zip,tar.gz)"

# ---- tests ----
test: headless
	$(PYTHON) tests/run_tests.py

cosim: headless headless-recomp
	$(PYTHON) tests/cosim.py

clean:
	rm -rf $(BUILD)
