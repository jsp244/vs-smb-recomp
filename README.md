# VS. Super Mario Bros. native PC port (static recompilation)

A native Windows, Linux, Steam Deck and Android build of Nintendo's arcade *VS. Super
Mario Bros.* (Vs. System, 1986), produced by **statically recompiling your own dump of
the board** into C and linking it against a small hardware runtime (PPU with the
arcade's RGB palette, APU, the Vs. System's mapper, coin slots and DIP switches), the
same way as the StarTropics port this one is built from.

This repository contains **no game data and no game code**. The recompiler, the
runtime and the frontend are original work (MIT licence). The game's 6502 program,
graphics, music and level data all come from the dump you supply; the C code
generated from it lives in `generated/`, which git ignores, and the executable loads
the dump again at start-up for the assets. Without your dump nothing here runs.

```
your VS. Super Mario Bros. dump ──► recomp/recomp.py ──► generated/*.c ─┐
 (.nes, or MAME's suprmrio.zip)                                          ├─► gcc ─► build/vssmb
runtime/ (PPU, APU, Vs. System, CPU fallback) + frontend/ (SDL2) ───────┘
                 ▲
your dump again, at run time, for the graphics, the level data (kept in CHR ROM on this board) and music
```

## For players: the launcher

`make dist` packages a single executable (SDL2 linked statically, no DLLs) with a
launcher in the style of the N64 recompilation projects: a tabbed menu
(**Play / Graphics / Audio / Controls / Cabinet / ROM**) over the game's own title
screen and demo, running with sound behind it. On the ROM tab "Choose ROM file..."
opens the system file dialog; dropping the file onto the window or passing it on the
command line also works. Both an iNES `.nes` file and **MAME's `suprmrio.zip` set**
are accepted: the six chips are found in the zip by their CRC32 and put together in
memory. The hash is checked and anything that is not a known VS. Super Mario Bros.
dump is refused.

**The cabinet.** VS. Super Mario Bros. is a coin-op: it has a coin slot, 1P and 2P
start buttons and DIP switches on the board. The **Cabinet** tab sets the switches
(read by the game when it starts): coinage (**free play**, the default, or 1-3 coins
per credit, 1-5 credits per coin), lives (3 / 2), coins for an extra life (100 / 150 /
200 / 250), the timer (normal / fast) and lives after a continue (4 / 3). Select is
**Insert coin** (one coin per press; the board refuses a coin held in the slot too
long, so the port drops it for exactly four frames), Start is the 1P start button,
and 2P start has its own binding (the 2 key). In a two-player game the controller
goes to whoever's turn it is, so one pad can be passed between turns as on the NES.

**Colors.** The Vs. board's RP2C04-0004 PPU drives an RGB monitor through a
scrambled palette (copy protection). The port carries that PPU's table and its RGB
levels (from the NESdev wiki and MAME, which agree on all 64 entries; checked against
this game: its four level palettes give exactly the NES version's colours), so
*Arcade (RGB monitor)* is what the cabinet showed. *NES (composite)* maps each colour
to the NES console's composite palette instead.

Options (all saved to `vssmb.ini`): windowed / fullscreen / exclusive fullscreen, a
resolution list built from the display's own modes plus 1920x1080 and 3840x2160,
window size 1x-6x, scaling (integer / fit / stretch), aspect ratio (square pixels 8:7,
4:3 CRT, 16:9 or 16:10 widescreen: below), colours (arcade / NES), filter (nearest /
linear / sharp), CRT-style
shaders (scanlines, aperture mask, glow, curvature; three strengths), overscan crop,
a no-sprite-limit mode (no flicker), volume, and rebinding of every button. Nine save
state slots and rewind (hold a key to step back through the last minutes of play)
are built in. There are no RetroAchievements: no set exists for this game. Esc in
game opens the same menu as a pause screen.

**16:9 (widescreen)** and **16:10 (widescreen)** are real widescreen views, not a
stretch, as in the StarTropics port. The NES picture is 256 pixels wide, but the
game keeps a 512-pixel window of the level in its two nametables (it renders 90-128
pixels ahead of the right edge, and what scrolled out on the left stays there), so
the port draws those columns beside the picture with the scroll, CHR bank and
palette each line was drawn with: the level simply continues past both edges, with
no seam, and the status bar keeps its place. Where the game holds no level (before
the start of an area, past what it has rendered, the intermission and the text
screens) the picture sits between black bars. The pixels keep their 4:3 CRT shape:
a 16:9 picture shows 341 columns (319 with the overscan crop). With Integer scaling
in fullscreen the picture fills the whole screen at the largest whole multiple, the
level beside it (2560x1600 at 7x on a 16:10 laptop panel).

**Objects in the bands.** The game creates an enemy when it comes within 48 pixels
of the right edge and erases it 72 pixels past either edge, so the bands hold live
Goombas, Koopas, Piranha Plants, Lakitu, Bloopers and the rest; but it only builds
sprites for its own 256-pixel screen. The port draws them anyway with the game's own
code: when the frame's object code (GameEngine) starts, a hook copies RAM, and that
copy runs GameEngine again in a CPU sandbox (no hardware access, no time passing)
with the camera moved 112 pixels towards each band. Every object is processed and
drawn exactly as in the real frame, only placed on a wider screen, and its sprites go
into the band with the PPU's priority rules. The 8-pixel sprite columns the game hides
where an object straddles an edge are drawn back, so objects cross into and out of
the picture whole. In 16:9 an enemy walks in from the physical edge of the screen
(the game creates it just beyond a 42-pixel band); in wider Integer views it appears
inside the band, 48 pixels past the old edge.

**After a death or a pipe.** An area entered part-way (at its halfway point after a
death, or from a pipe) is drawn by the game from that page on, so the columns left of
the screen were never rendered. The port draws that page the way the game draws an
area: InitializeArea, from a copy of RAM made as the area starts, runs again in the
sandbox told to start a page earlier, and the area parser renders the page into a
copy of the nametables, which the left band reads.

Note on distribution: a prebuilt executable necessarily embeds the recompiled game
logic (as the N64 recompilation releases do), while every asset still comes from the
player's own dump at run time. Building from source as described below keeps
everything derived from the ROM on your own machine.

## What works

Verified (headless harness with scripted input and RAM pokes, screenshots inspected):

* boot, title screen with INSERT COIN and the top score, the attract demo
* coin slot (credits, the coin counter, a coin held too long refused), 1P start,
  the "PUSH BUTTON" screen, free play, game over and the continue countdown
* every world 1-8 (each started by poking the world and area number and reloading it):
  overworld, underground, underwater, castles with firebars and lava, Lakitu stages
* the recompiled code is **bit-identical** to the reference interpreter: a 9,000-frame
  coin-op session, a 6,000-frame free-play session, and 40 level runs (8 worlds x 5
  areas, 120,000 frames), comparing CPU registers and cycle counts, RAM, work RAM,
  framebuffer, OAM, palettes, nametables and bank registers every frame; none of it
  fell back to the interpreter (static analysis finds 12,156 instructions)
* the interpreter passes nestest (all 8,991 instructions) and 44 of the 63 blargg
  accuracy test ROMs (the failures are listed in `docs/ARCHITECTURE.md`)
* the frontend self-tests: run-ahead determinism, rewind (every re-run frame matches,
  forward again after a rewind), the Start + Select rollback, the trigger chord
* a real window on a 240 Hz laptop panel: vblank phase-lock, 60 fps, input to present
  about 2 ms, no missed deadlines
* widescreen: the bands are the game's own nametable columns (the centre re-rendered the
  same way differs from the PPU's frame only by the sprites, 0.2% of the pixels in 1-1);
  16:9 in a window and Integer filling a 2560x1600 screen checked by screenshots
* objects in the bands: GameEngine re-run in the sandbox with the camera where it was
  builds exactly the sprites the PPU shows in all 62,688 frames of play across the 40
  level runs (`--ws-oam-check`); the picture between the 8-pixel edge columns is
  unchanged; seams checked at both edges (Goombas, Koopas, Buzzy Beetles, a Blooper,
  Piranha Plants); the page before a halfway respawn drawn into the left band

Not yet verified: a full play-through to the ending by a person, the name entry for a
top score, and two-player games beyond starting one.

## Legal note

VS. Super Mario Bros. and Super Mario Bros. are trademarks and copyrights of
Nintendo. You need your own legally obtained dump of the board; the project does not
include, download or write one. Everything derived from your ROM (the generated C,
disassembly listings, builds, saves) stays on your machine. Do not redistribute
`generated/` or the built executable, since those contain the recompiled game.

## Building

Requirements: a C11 compiler (gcc/clang), GNU make, Python 3, SDL2 (frontend only).

**Windows (MSYS2)**: install [MSYS2](https://www.msys2.org), open the *MINGW64* shell and

```
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make python
```

**Linux**: `sudo apt install build-essential python3 libsdl2-dev` (or your distro's
equivalent) for a local build; portable packages are built in a container (below).

Then:

```
git clone <this repo> vs-smb-recomp
cd vs-smb-recomp
cp "/path/to/VS Super Mario Bros (VS).nes" rom/   # your own dump (or MAME's suprmrio.zip)
make recomp            # disassemble + translate your ROM -> generated/*.c  (a few seconds)
make -j8 vssmb         # compile the generated C + runtime + frontend (~20 s)
build/vssmb            # looks for rom/*.nes or *.zip next to the executable; or pass a path
```

`make recomp` prints how many instructions and routines it found (12,156 instructions
in 261 routines from static analysis alone: the recursive descent follows the game's
jump tables, see `recomp/symbols.txt`). Run it again after updating `recomp/` (the
generated code carries the hooks `recomp/symbols.txt` marks; without them the
widescreen bands show the level but not its objects). `make dist` zips the
executable with the player notes (`docs/PLAYERS.md`) into `dist/`.

### Linux and Steam Deck packages

`tools/linux/Dockerfile` is an Ubuntu 22.04 build environment with SDL2 built from
source as a static library that loads X11, Wayland, PipeWire, PulseAudio, ALSA and
udev at run time. The resulting binary needs only glibc 2.34+ and libm, so it runs on
SteamOS 3 and current distributions without installing anything. After `make recomp`:

```
docker build -t vssmb-linux tools/linux
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/src -w /src vssmb-linux sh tools/linux/build.sh
```

This builds, runs the cosimulation and the frontend self-tests headless (SDL's
offscreen driver), and writes `dist/vssmb-linux-x86_64.tar.gz` and
`dist/vssmb-steamdeck.tar.gz`. The Steam Deck edition (`make DECK_EDITION=1`) starts
fullscreen at the Deck's 1280x800 in the 16:10 widescreen view with pad-first hints;
the generic Linux build does the same when it finds itself on a Deck. Its package
carries `add-to-steam.py`, which adds the game to Steam with library artwork from
SteamGridDB. Setup on the Deck: `docs/STEAMDECK.md`.

### Android

`android/build_apk.py` builds two APKs straight from the Android SDK tools (no Gradle):
`dist/vssmb-android-handheld.apk` for gaming handhelds with built-in controls and
`dist/vssmb-android-phone.apk` for phones, the same app with a translucent on-screen
controller (`frontend/touchpad.c`: d-pad, B, A, COIN, START, MENU) under its own name
("VS. Super Mario Bros. Touch"). They hold SDL2's Java glue and native library from the
SDL source (fetched once), the game as `libmain.so` for arm64-v8a and x86_64 (NDK +
CMake), a small activity for the system file picker, and launcher icons drawn from the
port's own artwork; the phone edition differs only in its manifest. The app has no
internet permission. It needs a platform 34+, build-tools, an NDK and CMake from the SDK
manager, plus a JDK; both APKs are signed with a key kept in
`~/.android/vssmb-release.keystore` (created on first use).

```
python android/build_apk.py                           # after make recomp
python android/build_apk.py --edition phone --install # one of them, and adb install it
```

Player notes for handhelds and phones: `docs/ANDROID.md`; device test tools (a virtual
controller for adb, frame-timing summary): `tools/android/`.

### Controls

| Cabinet | Keyboard | Gamepad (SDL) |
|---|---|---|
| Stick | Arrow keys | D-pad / left stick |
| A (jump) | X | A (bottom face button) |
| B (run, fireball) | Z | X (left face button: B sits left of A, as on the NES pad) |
| Insert coin | Right Shift | Back |
| 1P start | Enter | Start |
| 2P start | 2 | (unbound) |

Keys and pad buttons are rebindable on the Controls tab (select a row, press Enter,
then the new key or pad button). Every connected controller is opened and merged.

### Latency

See [docs/LATENCY.md](docs/LATENCY.md). In short: the loop is phase-locked to the
display's vertical blank and reads the controller about 2 ms before each frame is
presented (frame delay), the GPU queue is one frame deep, **run-ahead** (default 1
frame, the game's own reaction time, measured) shows the game's response on the frame
of the press, and audio sits at 25-45 ms with dynamic rate control. Exclusive
fullscreen bypasses the desktop compositor. `vssmb --play --stats out.txt` logs the
measured pacing once a second. On Android each frame is locked to the compositor's
frame deadlines (Choreographer), with the display switched to 120 Hz where it can; on
Linux and the Steam Deck frames go out without vsync, as in Mesen by default, unless
Low-latency sync is switched off.

Hotkeys: **Esc** or **Start + Insert coin** together (pad or keyboard) menu, **LT + RT**
together on Android and the Steam Deck, **F11** /
Alt+Enter fullscreen, **F2** toggle overscan crop, **Tab** / pad RT (hold)
fast-forward (within 1.5 s after a rewind it first goes forward again to where the
rewind began), **P** pause, **F5** / **F8** save / load state, **F6** / **F7** previous
/ next slot (nine per ROM), **Backspace** (hold; pad RB) rewind, **F12** screenshot
(`screenshots/`), **Ctrl+R** reset (the board reads its DIP switches again).

Save states go to `saves/<rom name>.state1` .. `.state9`.

## How it works

* `recomp/recomp.py` reads the iNES header (or the MAME zip), and with 32 KiB of
  fixed PRG translates the whole of `$8000-$FFFF` as one unit: recursive descent from
  the reset/NMI/IRQ vectors, through the word tables after each `JSR` to the game's
  jump-table dispatcher (`JumpEngine` in `recomp/symbols.txt`), optionally seeded with
  execution coverage from the headless harness. Every 6502 instruction becomes a
  `case` label, so execution can resume at any instruction after an interrupt or an
  indirect jump.
* `runtime/` is the hardware: a dot-accurate PPU (sprite 0 hit drives the status-bar
  split), a cycle-stepped APU with the NES mixer and output filters, the Vs. System
  board (mapper 99: CHR bank from `$4016` bit 2, 2 KiB of work RAM at `$6000`, coin
  and service inputs and DIP switches on `$4016`/`$4017`, the coin counter on
  `$4020`), the RP2C04 palettes, and a 6502 interpreter that doubles as the fallback
  for untranslated code and as the reference model.
* `frontend/sdl_main.c` is the player: window, audio, input and the cabinet's
  controls, saves, states, rewind, shaders.

Details are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing

```
sh tests/get_test_roms.sh          # clones the public nestest/blargg collection (33 MB)
make headless                      # interpreter-only harness
make test                          # blargg suites through $6000 status reporting
make headless-recomp               # harness with the recompiled code linked in
python tests/cosim.py              # interpreter vs native, a 9,000-frame coin-op session
python tests/cosim.py --levels     # interpreter vs native in every world (about 3.5 minutes)
build/nes-headless-recomp --rom rom/*.nes --recomp --frames 9000 --ws-oam-check
                                   # the re-run engine must build the PPU's sprites exactly
sh tests/run_ui_bind_test.sh       # menu rebinding and text rows (synthetic events)
build/vssmb rom/*.nes --rewind-test 1200   # whole rewind path: every re-run frame must match
build/vssmb rom/*.nes --chord-test         # the Start + Select rollback
```

The headless harness (`build/nes-headless --help`) scripts input
(`--keys "300:COIN;304:-;400:SELECT"`; `2A`, `2SELECT` ... for the second controller),
sets DIP switches (`--dip 07` = free play), writes memory before a frame (`--poke
"405:75F=3,760=0"`), dumps PNG screenshots every N frames, writes WAV audio, records
coverage (`--coverage FILE`), traces every instruction and dumps RAM.

## Repository layout

```
recomp/        the static recompiler (Python) and symbols.txt (address labels, no game data)
runtime/       hardware runtime + headless harness (C11), the MAME zip reader
frontend/      SDL2 player (menu, video, audio, input, touch controls, rewind)
android/       Android app: CMake project, manifest, activity, build_apk.py
tests/         accuracy tests, cosimulation, nestest comparison, module tests
tools/         icon generator, Linux / Steam Deck build container (tools/linux), Steam
               shortcut script (tools/steamdeck), Android device test tools, DXGI adapter lister
docs/          architecture notes, player notes (Windows/Linux, Steam Deck, Android), latency notes
generated/     output of make recomp (git-ignored)
rom/           your dump (git-ignored)
```
