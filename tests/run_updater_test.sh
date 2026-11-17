#!/bin/sh
# Builds and runs the updater's offline test (tests/updater_test.c), as built for Windows, Linux or
# macOS, and on Windows and Linux then as the Steam Deck edition (whose notes include STEAMDECK.txt).
# The packages it unpacks are made here the way `make dist` makes them (Python's
# shutil.make_archive), from stand-in programs and notes, with the Deck package's other files beside
# them (only the program and the notes may be written); the macOS package holds a stand-in app.
# SDL_LIBS overrides how SDL is linked (tools/linux/build.sh, tools/macos/build.sh: their static library).
set -e
cd "$(dirname "$0")/.."
D=build/updater-test
rm -rf "$D"
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
"$PY" - "$D" <<'EOF'
import os, random, shutil, sys, zipfile
d = sys.argv[1]
random.seed(7)
# one tree per package: MSYS2's Python treats "vssmb" and "vssmb.exe" as the same file
for kind, name, head, fmt in (("win", "vssmb.exe", b"MZ", "zip"), ("linux", "vssmb", b"\x7fELF", "gztar"),
                              ("mac", "VS. Super Mario Bros.app/Contents/MacOS/vssmb", b"\xca\xfe\xba\xbe", "zip")):
    folder = os.path.join(d, kind, "vssmb")
    os.makedirs(os.path.dirname(os.path.join(folder, name)), exist_ok=True)
    # incompressible bytes (stored blocks) and long runs (Huffman-coded blocks)
    data = head + bytes(random.getrandbits(8) for _ in range(300000)) + b"\0" * 200000 + b"VS. Super Mario Bros. " * 5000
    with open(os.path.join(folder, name), "wb") as f:
        f.write(data)
    if kind == "mac":   # the rest of the stand-in app
        os.chmod(os.path.join(folder, name), 0o755)
        app = os.path.join(folder, "VS. Super Mario Bros.app", "Contents")
        os.makedirs(os.path.join(app, "Resources"))
        with open(os.path.join(app, "Info.plist"), "w", newline="\n") as f:
            f.write("new plist\n")
        with open(os.path.join(app, "Resources", "vssmb.icns"), "wb") as f:
            f.write(b"icns\0\0\0\x08")
    for doc, text in (("README.txt", "notes\n"), ("STEAMDECK.txt", "deck notes\n"), ("add-to-steam.py", "#!/usr/bin/env python3\n")):
        with open(os.path.join(folder, doc), "w", newline="\n") as f:
            f.write(text)
    with open(os.path.join(folder, "vssmb.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
    shutil.make_archive(os.path.join(d, "mac" if kind == "mac" else "test"), fmt, os.path.join(d, kind), "vssmb")
# macOS packages that must be refused: a name that climbs out of the app, and no app at all
with zipfile.ZipFile(os.path.join(d, "mac-climb.zip"), "w") as z:
    z.write(os.path.join(d, "mac", "vssmb", "VS. Super Mario Bros.app", "Contents", "Info.plist"),
            "vssmb/VS. Super Mario Bros.app/Contents/Info.plist")
    z.writestr("vssmb/VS. Super Mario Bros.app/Contents/../../../escaped.txt", "x")
with zipfile.ZipFile(os.path.join(d, "mac-noapp.zip"), "w") as z:
    z.writestr("vssmb/README.txt", "notes\n")
EOF
CC=${CC:-gcc}
editions="'' -DDECK_EDITION"
[ "$(uname -s)" = Darwin ] && editions="''"
eval "set -- $editions"
for edition in "$@"; do
    $CC -O1 -std=gnu11 -Wall $edition -Ifrontend -Iruntime $(sdl2-config --cflags) \
        tests/updater_test.c runtime/romzip.c -o build/updater_test ${SDL_LIBS:-$(sdl2-config --libs)} -lm
    rm -rf "$D/run"
    mkdir -p "$D/run"
    ./build/updater_test "$D/test.zip" "$D/test.tar.gz" "$D" "$D/mac.zip"
done
