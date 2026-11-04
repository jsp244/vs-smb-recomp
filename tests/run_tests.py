"""Run the blargg-style test ROMs that report through $6000 and summarise.

usage: python tests/run_tests.py [filter]
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXT = os.path.join(ROOT, "tests", "external", "nes-test-roms")
EXE = os.path.join(ROOT, "build", "nes-headless.exe" if os.name == "nt" else "nes-headless")

SUITES = [
    ("instr_test-v5/rom_singles", 1500),
    ("instr_timing/rom_singles", 2500),
    ("cpu_interrupts_v2/rom_singles", 1500),
    ("ppu_vbl_nmi/rom_singles", 2500),
    ("apu_test/rom_singles", 1500),
    ("mmc3_test_2/rom_singles", 1500),
    ("cpu_dummy_writes", 1500),
    ("cpu_exec_space", 1500),
    ("oam_read", 1500),
    ("ppu_open_bus", 1500),
    ("ppu_read_buffer", 3000),
    ("apu_reset", 1500),
    ("cpu_reset", 1500),
    ("instr_misc", 1500),
]


def run(rom, frames):
    p = subprocess.run([EXE, "--rom", rom, "--frames", str(frames), "--blargg", "--quiet"],
                       capture_output=True, text=True, timeout=300)
    out = p.stdout.strip()
    status = None
    for line in out.splitlines():
        if line.startswith("blargg: status"):
            status = int(line.split()[-1], 16)
    return status, out


def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    passed = failed = 0
    for suite, frames in SUITES:
        d = os.path.join(EXT, suite)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".nes") or flt not in os.path.join(suite, name):
                continue
            status, out = run(os.path.join(d, name), frames)
            tag = "PASS" if status == 0 else ("TIMEOUT" if status is None else f"FAIL({status:02X})")
            print(f"{tag:10s} {suite}/{name}")
            if status != 0:
                failed += 1
                for line in out.splitlines()[1:6]:
                    print("    " + line)
            else:
                passed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
