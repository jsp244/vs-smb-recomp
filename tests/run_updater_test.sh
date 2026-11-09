#!/bin/sh
# Builds and runs the updater's offline test (tests/updater_test.c), as built for Windows or Linux
# and then as the Steam Deck edition (whose notes include STEAMDECK.txt). The packages it unpacks
# are made here the way `make dist` makes them (Python's shutil.make_archive), from stand-in
# programs and notes, with the Deck package's other files beside them (only the program and the
# notes may be written). SDL_LIBS overrides how SDL is linked (tools/linux/build.sh: its static library).
set -e
cd "$(dirname "$0")/.."
D=build/updater-test
rm -rf "$D"
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
"$PY" - "$D" <<'EOF'
import os, random, shutil, sys
d = sys.argv[1]
random.seed(7)
# one tree per package: MSYS2's Python treats "vssmb" and "vssmb.exe" as the same file
for kind, name, head, fmt in (("win", "vssmb.exe", b"MZ", "zip"), ("linux", "vssmb", b"\x7fELF", "gztar")):
    folder = os.path.join(d, kind, "vssmb")
    os.makedirs(folder)
    # incompressible bytes (stored blocks) and long runs (Huffman-coded blocks)
    data = head + bytes(random.getrandbits(8) for _ in range(300000)) + b"\0" * 200000 + b"VS. Super Mario Bros. " * 5000
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)
    for doc, text in (("README.txt", "notes\n"), ("STEAMDECK.txt", "deck notes\n"), ("add-to-steam.py", "#!/usr/bin/env python3\n")):
        with open(os.path.join(folder, doc), "w", newline="\n") as f:
            f.write(text)
    with open(os.path.join(folder, "vssmb.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
    shutil.make_archive(os.path.join(d, "test"), fmt, os.path.join(d, kind), "vssmb")
EOF
CC=${CC:-gcc}
for edition in "" -DDECK_EDITION; do
    $CC -O1 -std=gnu11 -Wall $edition -Ifrontend -Iruntime $(sdl2-config --cflags) \
        tests/updater_test.c runtime/romzip.c -o build/updater_test ${SDL_LIBS:-$(sdl2-config --libs)} -lm
    rm -rf "$D/run"
    mkdir -p "$D/run"
    ./build/updater_test "$D/test.zip" "$D/test.tar.gz" "$D"
done
