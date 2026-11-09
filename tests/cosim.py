"""Cosimulation check: run the same scripted session through the interpreter
and through the recompiled code and compare per-frame state hashes.

usage: python tests/cosim.py [--frames N] [--keys SPEC] [--dip HEX] [--rom FILE] [--reset F,F,...] [--levels | --walk N]

Exit code 0 means every frame matched (CPU registers and cycle count, RAM,
work RAM, framebuffer, OAM, palettes, nametables, bank registers).
--reset presses the reset button (as Play > Reset game) before those frames of the session.
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


# walking into the widescreen band left of the screen (runtime/bandwalk.c), free play in 1-1: into the
# band and waiting there for the first Goomba (a death beside the screen), the same with a stomp there
# and a run on through the level, walking left near the area's start (the wall at its first pixel), and
# after a restart at a halfway page (poked to 4 while the first Goomba kills him) walking into the page
# drawn before it, which gets its collision as the area starts; and that Goomba made a Koopa (its Enemy_ID
# poked as it appears), stomped in the band, its shell kicked there by the landing after the bounce: out
# through the picture, and (drifting right in the bounce, so he lands on its far side) out through the band
KOOPA = "400:SELECT;404:-;640:RIGHT;780:LEFT;880:-;1200:A;1206:-"
WALK_SCENARIOS = {
    "death": (1500, "400:SELECT;404:-;640:RIGHT;780:LEFT;1000:-", ""),
    "stomp": (2600, "400:SELECT;404:-;640:RIGHT;780:LEFT;1000:-;1234:A;1242:-;1320:RIGHT+B;1350:RIGHT+B+A;"
                    "1380:RIGHT+B;1420:LEFT;1520:RIGHT+B;1560:RIGHT+B+A;1600:RIGHT+B;1700:LEFT;1820:RIGHT+B+A;"
                    "1860:RIGHT+B;1940:LEFT+B;2040:RIGHT+B;2080:RIGHT+B+A;2120:RIGHT+B;2240:LEFT;2400:RIGHT+B+A;"
                    "2440:RIGHT+B", ""),
    "start": (1100, "400:SELECT;404:-;640:RIGHT;712:LEFT;1000:-;1040:RIGHT;1080:LEFT", ""),
    "halfway": (1900, "400:SELECT;404:-;640:RIGHT;1100:-;1350:LEFT;1500:-;1540:RIGHT;1560:LEFT;1600:-;"
                      "1640:RIGHT+B;1680:RIGHT+B+A;1720:RIGHT+B;1800:LEFT",
                ";".join(f"{f}:75B=4" for f in range(1000, 1130, 2))),
    "kick": (1600, KOOPA + ";1300:RIGHT+B;1340:RIGHT+B+A;1370:RIGHT+B", "762:16=0"),
    "kick-left": (1600, KOOPA + ";1226:RIGHT;1246:-;1300:RIGHT+B;1340:RIGHT+B+A;1370:RIGHT+B", "762:16=0"),
}


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
    ap.add_argument("--walk", type=int, default=0, metavar="N",
                    help="instead: walking into the widescreen band, N pixels beyond the screen's left edge "
                         "(runtime/bandwalk.c; the Deck's view has 42)")
    ap.add_argument("--reset", default=None, metavar="F,F,...",
                    help="the reset button before these frames of the session (as Play > Reset game)")
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
    if args.walk:
        ok = True
        for name, (frames, keys, pokes) in WALK_SCENARIOS.items():
            extra = ["--ws-walk", str(args.walk)] + (["--poke", pokes] if pokes else [])
            ok &= compare(interp, native, rom, frames, keys, "07", extra, f" walk {args.walk} {name}")
        sys.exit(0 if ok else 1)
    keys = args.keys or default_keys(args.frames)
    extra, label = (["--reset", args.reset], f" (reset before frame {args.reset})") if args.reset else ([], "")
    sys.exit(0 if compare(interp, native, rom, args.frames, keys, args.dip, extra, label) else 1)


if __name__ == "__main__":
    main()
