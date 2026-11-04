"""Cosimulation check: run the same scripted session through the interpreter
and through the recompiled code and compare per-frame state hashes.

usage: python tests/cosim.py [--frames N] [--keys SPEC] [--dip HEX] [--rom FILE]

Exit code 0 means every frame matched (CPU registers and cycle count, RAM,
work RAM, framebuffer, OAM, palettes, nametables, bank registers).
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""


def default_keys(frames):
    """attract mode, three coins (the second held too long, so the game refuses it), 1P start,
    then run right with B held, jumping in a varying rhythm, through deaths, game over and
    the continue screen"""
    ev = ["300:COIN", "304:-", "330:COIN", "360:-", "380:COIN", "384:-", "420:SELECT", "424:-"]
    f, k = 700, 0
    while f < frames:
        hold = 24 + (k * 7) % 19          # jump lengths vary, so different code paths are taken
        ev.append(f"{f}:RIGHT+B")
        ev.append(f"{f + 30}:RIGHT+B+A")
        f += 30 + hold
        k += 1
        if k % 40 == 0:                   # now and then the start button (continue, name entry)
            ev.append(f"{f}:SELECT")
            ev.append(f"{f + 4}:-")
            f += 30
    return ";".join(ev)


def level_keys(frames):
    """free play: 1P start, walk into the first enemy (the death reloads the level the pokes chose),
    then run and jump through it"""
    ev = ["400:SELECT", "404:-", "640:RIGHT"]
    f, k = 1150, 0
    while f < frames:
        hold = 20 + (k * 11) % 23
        ev.append(f"{f}:RIGHT+B")
        ev.append(f"{f + 26}:RIGHT+B+A")
        f += 26 + hold
        k += 1
    return ";".join(ev)


def level_pokes(world, area):
    # SMB's WorldNumber $075F, LevelNumber $075C (shown) and AreaNumber $0760 (which area of the world)
    return f"405:75F={world:X},75C={min(area, 3):X},760={area:X}"


def run(exe, rom, frames, keys, dip, out, extra):
    cmd = [exe, "--rom", rom, "--frames", str(frames), "--keys", keys, "--dip", dip, "--frame-hash", out, "--quiet"] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout, p.stderr)
        raise SystemExit(f"{exe} failed")


def compare(interp, native, rom, frames, keys, dip, extra, label):
    h1 = os.path.join(ROOT, "build", "cosim_interp.txt")
    h2 = os.path.join(ROOT, "build", "cosim_native.txt")
    run(interp, rom, frames, keys, dip, h1, extra)
    run(native, rom, frames, keys, dip, h2, extra + ["--recomp"])
    a = open(h1).read().splitlines()
    b = open(h2).read().splitlines()
    bad = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    n = min(len(a), len(b))
    if bad:
        print(f"MISMATCH{label}: first divergence at frame {bad[0]} ({len(bad)} of {n} frames differ)")
        print("Build a tracing harness to pinpoint it:")
        print("  make BUILD=build-trace RECOMP_DEFS=-DRECOMP_TRACE headless-recomp")
        print("  then run both harnesses with --trace FILE --trace-start <frame> and diff the traces")
        return False
    print(f"OK{label}: {n} frames bit-identical between interpreter and recompiled code")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=9000)
    ap.add_argument("--keys", default=None)
    ap.add_argument("--dip", default="00")
    ap.add_argument("--rom", default=None)
    ap.add_argument("--levels", action="store_true",
                    help="instead: every world, areas 0-4, each started through RAM pokes (free play, 3000 frames each)")
    args = ap.parse_args()
    rom = args.rom or (glob.glob(os.path.join(ROOT, "rom", "*.nes")) or [None])[0]
    if not rom:
        raise SystemExit("no ROM: put your .nes file in rom/ or pass --rom")
    interp = os.path.join(ROOT, "build", "nes-headless" + EXE)
    native = os.path.join(ROOT, "build", "nes-headless-recomp" + EXE)
    for exe in (interp, native):
        if not os.path.exists(exe):
            raise SystemExit(f"missing {exe}: run 'make headless headless-recomp' first")
    if args.levels:
        ok = True
        for world in range(8):
            for area in range(5):
                ok &= compare(interp, native, rom, 3000, level_keys(3000), "07",
                              ["--poke", level_pokes(world, area)], f" world {world + 1} area {area}")
        sys.exit(0 if ok else 1)
    keys = args.keys or default_keys(args.frames)
    sys.exit(0 if compare(interp, native, rom, args.frames, keys, args.dip, [], "") else 1)


if __name__ == "__main__":
    main()
