#!/bin/sh
# Fetch the public NES accuracy test ROM collection used by tests/run_tests.py.
# These are homebrew test programs (nestest, blargg's suites), not commercial games.
set -e
cd "$(dirname "$0")"
mkdir -p external
if [ ! -d external/nes-test-roms ]; then
    git clone --depth 1 https://github.com/christopherpow/nes-test-roms.git external/nes-test-roms
else
    echo "already present: tests/external/nes-test-roms"
fi
