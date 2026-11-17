#!/bin/sh
# Builds and runs the menu rebinding test (synthetic events, no window or controller needed).
# SDL_LIBS overrides how SDL is linked (tools/macos/build.sh: its static library).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC=${CC:-gcc}
$CC -O1 -std=gnu11 -Wall -Ifrontend -Iruntime $(sdl2-config --cflags) \
    tests/ui_bind_test.c frontend/ui.c frontend/font.c frontend/input.c \
    -o build/ui_bind_test ${SDL_LIBS:-$(sdl2-config --libs)} -lm
./build/ui_bind_test
