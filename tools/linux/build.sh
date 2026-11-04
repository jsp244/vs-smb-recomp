#!/bin/sh
# tools/linux/build.sh - builds and tests the Linux and Steam Deck packages. Run it inside the
# container made from tools/linux/Dockerfile (see there). Needs generated/ (make recomp);
# the tests also need your dump (.nes or MAME's suprmrio.zip) in rom/.
set -e
cd "$(dirname "$0")/../.."
J=$(nproc)
M="make -j$J PYTHON=python3 SDL_STATIC=1"

$M headless headless-recomp
$M BUILD=build-linux vssmb
$M BUILD=build-deck DECK_EDITION=1 vssmb

ROM=$(ls rom/*.nes rom/*.zip 2>/dev/null | head -n 1 || true)
if [ -n "$ROM" ]; then
    echo "== cosimulation (interpreter vs native code)"
    python3 tests/cosim.py --frames 3000
    # no display in a container: SDL's offscreen video and dummy audio drivers
    export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
    for b in build-linux build-deck; do
        echo "== $b self-tests"
        ./$b/vssmb "$ROM" --selftest 300 "/tmp/vs-$b.png" | grep selftest
        ./$b/vssmb --trigger-test | tail -n 1
        ./$b/vssmb --touch-test | tail -n 1
        ./$b/vssmb "$ROM" --chord-test | tail -n 1
        ./$b/vssmb "$ROM" --rewind-test 1200 | grep rewind-test
        ./$b/vssmb "$ROM" --ui-shot "/tmp/ui-$b.png" 1 60 | grep ui-shot
        grep -E "^(display_mode|aspect|scaling|resolution)=" $b/vssmb.ini | tr '\n' ' '; echo
    done
fi

echo "== shared libraries the binary needs"
ldd build-linux/vssmb
echo "== newest glibc symbol version it needs"
objdump -T build-linux/vssmb | grep -o 'GLIBC_[0-9.]*' | sort -V | uniq | tail -n 1

rm -f build-linux/vssmb.ini build-deck/vssmb.ini
rm -rf build-linux/saves build-deck/saves
$M BUILD=build-linux dist
$M BUILD=build-deck DECK_EDITION=1 dist
ls -l dist/
