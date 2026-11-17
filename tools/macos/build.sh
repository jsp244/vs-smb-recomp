#!/bin/sh
# tools/macos/build.sh - builds and tests the macOS package on a Mac with Xcode's command line tools,
# CMake and Python 3: SDL2 as a static library for Apple silicon and Intel Macs (once, into
# SDL_PREFIX), the game for both kinds of Mac, its tests, the app and dist/vssmb-macos.zip. Needs
# generated/ (make recomp); the tests also need your dump (.nes or MAME's suprmrio.zip) in rom/.
# Give the build its version as for Linux (see tools/linux/Dockerfile): VSSMB_VERSION and VSSMB_REPO.
# .github/workflows/macos.yml runs it on GitHub's Macs. The test pictures go to shots/.
set -e
cd "$(dirname "$0")/../.."
SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_PREFIX=${SDL_PREFIX:-$HOME/sdl2-$SDL_VERSION-universal}
J=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=11.0

if [ ! -x "$SDL_PREFIX/bin/sdl2-config" ]; then
    echo "== SDL $SDL_VERSION, a static library for arm64 + x86_64, into $SDL_PREFIX"
    t=$(mktemp -d)
    curl -fsSL -o "$t/sdl.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
    echo "$SDL_SHA256  $t/sdl.tar.gz" | shasum -a 256 -c -
    tar -xzf "$t/sdl.tar.gz" -C "$t"
    cmake -S "$t/SDL2-$SDL_VERSION" -B "$t/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
    cmake --build "$t/build" -j "$J"
    cmake --install "$t/build"
    rm -rf "$t"
fi
export PATH="$SDL_PREFIX/bin:$PATH"
SDL_STATIC_LIBS=$(sdl2-config --static-libs | sed -e 's/ -lm / /g' -e 's/ -lm$//')   # the test scripts add -lm themselves
M="make -j$J PYTHON=python3"

echo "== version ${VSSMB_VERSION:-dev}, repository ${VSSMB_REPO:-(none)}"
$M headless headless-recomp
$M BUILD=build-mac vssmb
echo "== the updater's offline test"
SDL_LIBS="$SDL_STATIC_LIBS" CC=clang sh tests/run_updater_test.sh | tail -n 1
echo "== the menu's rebinding test"
SDL_LIBS="$SDL_STATIC_LIBS" CC=clang sh tests/run_ui_bind_test.sh | tail -n 1

rm -rf shots && mkdir -p shots
ROM=$(ls rom/*.nes rom/*.zip 2>/dev/null | head -n 1 || true)
if [ -n "$ROM" ]; then
    ROM=$(cd "$(dirname "$ROM")" && pwd -P)/$(basename "$ROM")
    echo "== cosimulation (interpreter vs native code)"
    python3 tests/cosim.py --frames 3000
    python3 tests/cosim.py --frames 3000 --reset 2000
    python3 tests/cosim.py --walk 42
    echo "== self-tests with SDL's offscreen video, software renderer and dummy audio drivers"
    (
        export SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy   # (no OpenGL offscreen on a Mac)
        B=./build-mac/vssmb
        $B "$ROM" --selftest 300 shots/selftest.png | grep selftest
        $B --trigger-test | tail -n 1
        $B --touch-test | tail -n 1
        $B "$ROM" --chord-test | tail -n 1
        $B "$ROM" --rewind-test 1200 | grep rewind-test
        $B "$ROM" --reset-test 3000 | tail -n 1
        $B "$ROM" --ui-shot shots/ui-offscreen.png 1 60 | grep ui-shot
        $B --update-check | tail -n 2 || true   # the Updates tab's check (needs the network)
        if /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
            echo "== the Intel half of the program, under Rosetta"
            /usr/bin/arch -x86_64 $B "$ROM" --selftest 300 shots/selftest-x86_64.png | grep selftest
            /usr/bin/arch -x86_64 $B "$ROM" --rewind-test 600 | grep rewind-test
            cmp shots/selftest.png shots/selftest-x86_64.png && echo "the same picture from both halves"
        else
            echo "== (this Mac has no Rosetta: the Intel half was not run)"
        fi
    )
fi
rm -f build-mac/vssmb.ini
rm -rf build-mac/saves

echo "== the app and the package"
$M BUILD=build-mac dist
APP="build-mac/app/VS. Super Mario Bros.app"
printf 'architectures: '; lipo -archs "$APP/Contents/MacOS/vssmb"
echo "== the libraries it needs: macOS's own only"
otool -L "$APP/Contents/MacOS/vssmb" | tail -n +2
if otool -L "$APP/Contents/MacOS/vssmb" | tail -n +2 | grep -v -E '^[[:space:]]+(/usr/lib/|/System/Library/)'; then
    echo "a library that is not part of macOS (above)"; exit 1
fi
echo "== the oldest macOS it runs on"
otool -l "$APP/Contents/MacOS/vssmb" | grep -E '^ +minos ' | sort -u

echo "== the package unpacked as the Finder does: signature, Info.plist, icon"
U=$(mktemp -d)
ditto -x -k dist/vssmb-macos.zip "$U"
UAPP="$U/vssmb/VS. Super Mario Bros.app"
ls "$U/vssmb"
codesign --verify --deep --strict --verbose=2 "$UAPP"
plutil -lint "$UAPP/Contents/Info.plist"
iconutil -c iconset -o "$U/icon.iconset" "$UAPP/Contents/Resources/vssmb.icns"
ls "$U/icon.iconset" | tr '\n' ' '; echo

if [ -n "$ROM" ]; then
    echo "== the app in a real window (Cocoa, Metal), as unpacked"
    BIN="$UAPP/Contents/MacOS/vssmb"
    SUPPORT="$HOME/Library/Application Support/VSSMBPort/VSSuperMarioBros"
    rm -rf "$SUPPORT"
    "$BIN" "$ROM" --game-shot "$PWD/shots/game-window.png" 300 | grep -E 'renderer|pacing|game-shot'
    "$BIN" "$ROM" --ui-shot "$PWD/shots/ui-window.png" 1 60 | grep ui-shot
    perl -e 'alarm 60; exec @ARGV' "$BIN" --dialog-test
    test -f "$SUPPORT/vssmb.ini" && echo "settings: in Application Support"
    echo "== opened by Launch Services with the ROM dropped on its icon, then closed as macOS closes it"
    rm -f "$SUPPORT/vssmb.ini"
    open -a "$UAPP" "$ROM"
    for i in $(seq 1 30); do grep -q '^rom=' "$SUPPORT/vssmb.ini" 2>/dev/null && break; sleep 1; done
    screencapture -x shots/screen.png 2>/dev/null || true
    pkill -TERM -x vssmb || true
    for i in $(seq 1 20); do pgrep -x vssmb >/dev/null || break; sleep 1; done
    grep '^rom=' "$SUPPORT/vssmb.ini"
    test "$(sed -n 's/^rom=//p' "$SUPPORT/vssmb.ini")" = "$ROM" && echo "the dropped ROM was loaded and remembered"
    echo "== nothing was written into the app"
    codesign --verify --deep --strict "$UAPP" && echo "signature still intact"
    rm -rf "$SUPPORT"
fi
rm -rf "$U"
ls -l dist/
