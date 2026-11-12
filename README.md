# VS. Super Mario Bros. native PC port (static recompilation)

A native Windows, Linux, Steam Deck and Android build of Nintendo's arcade *VS. Super
Mario Bros.* (Vs. System, 1986), produced by **statically recompiling your own dump of
the board** into C and linking it against a small hardware runtime (PPU with the
arcade's RGB palette, APU, the Vs. System's mapper, coin slots and DIP switches).

This repository contains **no game data and no game code**. The recompiler, the
runtime and the frontend are original work (MIT licence), but for the SNES sound unit
of the All-Stars music (`runtime/spc700.c`), which is LakeSnes's emulation (also MIT; its
notice is in the file). The game's 6502 program,
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
(**Play / Graphics / Audio / Controls / Cabinet / ROM / Updates**) over the game's own title
screen and demo, running with sound behind it. On the ROM tab "Choose ROM file..."
opens the system file dialog; dropping the file onto the window or passing it on the
command line also works. An iNES `.nes` file (on its own or in a `.zip`) and **MAME's
`suprmrio.zip` set** are accepted: the six chips are found in the zip by their CRC32
and put together in memory (merged and split sets work too). The hash is checked and
anything that is not a known VS. Super Mario Bros. dump is refused. Two are known: the
usual one and MAME's `suprmrioa` (the "[a1]" dump), whose timer runs faster: it ticks
every 17 frames instead of 20 (13 instead of 17 with the fast-timer switch). Those two
operands are the only difference between them, so the generated code reads just those
from the dump and runs either natively.

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

**Remastered graphics (Super Mario All-Stars).** With your own *Super Mario All-Stars*
ROM (SNES, USA; also the Wii and Switch Online copies of it, and *Super Mario All-Stars +
Super Mario World*, USA) the game can be shown with
the graphics of All-Stars' Super Mario Bros.: on the ROM tab "Choose All-Stars ROM..."
(or drop the `.sfc`, or a `.zip` with it, onto the window, or `--allstars FILE`), then **Graphics > Graphics:
Arcade / All-Stars**. The ROM is hash-checked and remembered; nothing from it is stored
anywhere but in memory. The picture is drawn by All-Stars' own code, running in a small
SNES sandbox: its start-up, its InitializeArea and area graphics setup, and its area
parser, fed VS.'s own level data column by column, so the blocks, pipes, castles and
scenery are All-Stars' choice for VS.'s levels (VS.'s own levels too), with the parallax
hills and clouds, the sky gradient, the animated tiles, the status bar in All-Stars'
font, the intermission with its world picture, and All-Stars' sprites and palettes for
the player (fire, Luigi, the star's cycle), enemies, items, fireballs, coins, hammers
and blocks. What All-Stars has no art for is drawn the way All-Stars would have it: the
title's banner is All-Stars' with "VS." in front (its S and dot, and a V made from its
U by the rule that makes the arcade's V from the arcade's U), the coin, credit,
PUSH BUTTON, continue, game-over, high-score and name-entry screens use All-Stars'
backdrop and font in the arcade's text colours, and the cabinet's buttons, the timer's
box, the high scores' brick frames and the players' heads there are the arcade's pixels
lit and outlined in All-Stars' manner. The widescreen views work with it (the objects beside
the picture and the walk into the left band included). All-Stars also dressed its own
levels with objects VS.'s level data does not have: its castles' first staircase as
smooth steps, rounded ends wherever a floor stops at a gap, steps between floors of two
heights, caps on the wall's bricks where the wall stops, coral on the water levels'
terrain, stone blocks over the floors its castles leave hanging in mid-air. It put them
wherever the level's shape asks for them, so the port finds those places in VS.'s levels
(its own castles too) and draws the same tiles there.

**All-Stars' music.** The same ROM brings All-Stars' music, chosen apart from the
graphics: **Audio > Music: As the graphics / Arcade / All-Stars** (As the graphics, the
default, plays All-Stars' with the All-Stars graphics and the arcade's with the
arcade's). All-Stars' own sound program plays it: its sound engine, its instruments
(the directory and samples it loads at its title screen) and its Super Mario Bros.
songs, written from the ROM into an emulated SNES sound unit (`runtime/spc700.c`,
LakeSnes's SPC700, timers, ports and S-DSP, run cycle by cycle), which gets the song
All-Stars asks for whenever VS. starts music (Bowser's battle song included, as in
All-Stars). The arcade's sound effects stay: while All-Stars' music plays, the APU's
channels sound only while an effect uses them. It costs about 0.2 ms a frame on a
laptop CPU.

Options (all saved to `vssmb.ini`): windowed / fullscreen / exclusive fullscreen, a
resolution list built from the display's own modes plus 1920x1080 and 3840x2160,
window size 1x-6x, scaling (integer / fit / stretch), aspect ratio (square pixels 8:7,
4:3 CRT, 16:9 or 16:10 widescreen: below), colours (arcade / NES), filter (nearest /
linear / sharp), CRT-style
shaders (scanlines, aperture mask, glow, curvature; three strengths), overscan crop,
a no-sprite-limit mode (no flicker), volume, and rebinding of every button. Updates
from the GitHub releases (the Updates tab, below), nine save
state slots and rewind (hold a key to step back through the last minutes of play)
are built in. There are no RetroAchievements: no set exists for this game. Esc in
game opens the same menu as a pause screen.

**16:9 (widescreen)** and **16:10 (widescreen)** are real widescreen views, not a
stretch. The NES picture is 256 pixels wide, but the
game keeps a 512-pixel window of the level in its two nametables (it renders 90-128
pixels ahead of the right edge, and what scrolled out on the left stays there), so
the port draws those columns beside the picture with the scroll, CHR bank and
palette each line was drawn with: the level simply continues past both edges, with
no seam, and the status bar keeps its place. Where the game holds no level (before
the start of an area, past what it has rendered, the intermission and the text
screens) the picture sits between black bars; after a castle's loop (the wrong path
sends Mario four pages back) the columns left of the area's start are the level from
before the loop, and the bands show them. The pixels keep their 4:3 CRT shape:
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
copy of the nametables, which the left band reads. With the remastered graphics,
All-Stars' own parser renders that page the same way.

**Walking into the left band.** The game stops Mario at the left edge of its 256-pixel
screen, even where a widescreen view shows the level further left. The port lets him
walk on to the edge of the picture you see, as far as the band shows the level: 32
pixels in a 16:9 window, 16 in the 16:10 view the Deck starts in, 42 with Integer
filling the Deck's screen, 64 on a 1920x1080 one (at most 104). The screen still never
scrolls back. Enemies and power-ups work there as on the screen: the game's own
collision code decides a stomp, a hit, a kicked shell or a mushroom, run with both objects
moved to where its 8-bit screen positions do not wrap. His fireballs fly and bounce
there and hit what they meet, enemies there bump each other and a kicked shell knocks
them over, a vine there stays and can be climbed, firebars there burn as on the screen,
and the coins and brick pieces of a block hit there show there. A score earned there
shows at the screen's left edge. After a halfway restart or a pipe, the page drawn
before the start gets its ground as well (the game itself only knows the level from the
start page on). Not covered: riding a platform into the band, and hammers and platforms
touching him while he is wholly in it; the game still erases things 72 pixels past the
edge (only the widest views show that far). `walk_to_edge=0` in `vssmb.ini` keeps
the game's own edge. A few instructions marked `patch` hand the game's state to
`runtime/bandwalk.c` (`docs/ARCHITECTURE.md` has the details).

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
  Piranha Plants); the page before a halfway respawn drawn into the left band; Mario
  drawn once (the re-run makes his bounding box with the real camera and every other
  object's with the moved one, so it had put a second Mario in a band while he rode a
  lift, beside an enemy and on an area's first frame: those went, and nothing else changed
  in 151,098 frames of 52 sessions)
* walking into the left band (`tests/cosim.py --walk N`, N = 42, 64, 104): in and back,
  a Goomba's hit and a stomp there, the wall at an area's start, standing on the drawn
  page before a halfway restart, a Koopa stomped there and its shell kicked out through
  the picture or out through the band (stomp, kick, slide, points and the touching bit
  frame for frame as the same stomp and kick in the picture) and knocking over a Goomba
  there; Fire Mario's fireballs thrown both ways there, one hitting a Goomba there; a vine
  scrolled into the band and climbed there; a brick broken there by big Mario; firebars
  (one 256 pixels to his right, which must not touch him, and one in the band, which
  does); bit-identical between both paths with the patches working, the band's
  sprites still the PPU's exactly (`--ws-oam-check`), and nothing changed with them off
  (the 9,000-frame session and the 40 level runs); a real 16:9 window and the Deck's
  1280x800 views (Fit 16 pixels, Integer 42) checked by screenshots

* the remastered graphics, compared frame by frame with the arcade picture in all 32
  levels (each started by poking the world, level and area; star power and run-and-jump
  input): the overworld by day, night and snow, underground, underwater, the castles
  (lava, fire bars, lifts), the treetop and mushroom levels, Lakitu, Bullet Bill and
  Hammer Bro stages; the title with its banner and the demo scrolling it away, the
  PUSH BUTTON, intermission, game-over and continue screens, the attract's high scores
  and the name entry for a top score (poked; and after the game's end, the cursor over
  every letter, RUB and END, then the coin screen at one to four credits, the arcade's
  pixels on them checked pixel by pixel every frame in 4:3 and 16:9), every frame of the
  attract and of the coin-op session drawn (the screen's off frames black); 16:9 with the objects
  beside the picture, the walk into the left band (Mario small, big and fiery whole as he
  crosses the picture's edge, in 16:9 and in the Deck's 16:10 Integer view, as are the
  objects crossing either edge) and, under water, the tint and the
  gradient carried on into the bands; after a halfway restart or a pipe, the page before
  the start in the left band, parsed by All-Stars' parser, where and when the arcade's
  band draws its own (132 part-way starts: black only where the arcade's band is black,
  nothing else changed), also after a rewind or a load in the same session; the warp zone's pipes in
  All-Stars' art down to the black screen, as All-Stars draws them, and with them the
  frames at a level's or a world's end and a two-player handover that showed the arcade
  picture; after a state load, a rewind or fast-forward step and the Start + Select
  rollback the area of the loaded moment rebuilt (96 jump tests over 19 scenes and loads
  every frame through area starts and castle loops: the level as in the straight run, no
  arcade picture), holding rewind costing about what drawing does; 8-4's lift along the
  lava in front of it, as in the arcade (a falling lift sinks in, as in All-Stars); the
  intermission compared with
  All-Stars' own (run in a reference emulator), the castles' with Bowser breathing fire
  in its picture; the night sky's clouds and the floating score numbers as All-Stars
  draws them (a stomped or kicked shell stays on the ground under its points); the castle loops of 5-4, 7-4 and 8-4, where the blocks, the background
  and the bands carry on through the jump back as in All-Stars, frame by frame from the
  loop's first picture (in 4:3, 16:9 and 16:10, and after a state load before, during or
  after the loop: the background no longer jumps to its new place and back); All-Stars' own decorations
  (staircases, floor ends and steps, wall caps, coral, the crowns erased, the blocks over
  5-4's and 7-4's hanging floors) on every VS. area, compared cell by cell with All-Stars'
  own areas parsed by its own parser (in the world each is played in) where the level is
  SMB's: 1,140 cells as All-Stars draws them and none wrong, the rest of the difference
  All-Stars' own slips, and no cell changed between solid and empty; 8-4's water area
  compared with All-Stars' own in the reference emulator (its stone, caps and floor), its
  coral above the pipe out (a wall in All-Stars' own) drawn as on the other water levels;
  the level ends and the objects All-Stars
  draws its own way, each reached with RAM pokes (a start page, star power, an object
  spawned) and compared with All-Stars in the reference emulator: the goal flag, the
  castle's star flag and fireworks, Bowser walking, breathing fire and falling upside
  down, the bridge taken away under him after the axe in all eight castles (each piece
  on the arcade's frame, All-Stars' rubble sprite for sprite), his roar from the bridge's
  first piece through his fall (All-Stars' frames 6 and 5 by turns, frame for frame with
  All-Stars in 1-4, 6-4 and 8-4, facing either way), his flame, the castle's
  end with Toad's sack (1-4) and the princess (8-4), the game's end with its seven Toads,
  Podoboos, Bullet Bills, shells still and kicked, a vine, the cloud lift, springboards,
  Bloopers, Cheep-cheeps, Hammer Bros, Lakitu and Spinies, defeated enemies falling upside
  down, the water's surface; blocks hit from below in 1-1 and 1-2 (a ? block with a coin, the
  mushroom's ? block with the mushroom rising behind the used block, a brick bumped by small
  Mario and broken by big Mario) frame by frame against the arcade, with the used block and
  the bouncing block's place as All-Stars' own code writes them; the player going into the
  castle after each of the 20 flagpoles, small, big and fiery, gone on the arcade's frame (in
  4:3 and 16:9); every level's columns
  checked against VS.'s block buffer by a debug self-check (all 32 levels, with deaths and
  the restarts at the halfway page: no cell different). The emulation is untouched by it: per-frame
  state hashes are identical with and without it (and with the band walk, and through a
  castle loop), and the
  frontend self-tests pass with it on. The Linux build draws it pixel for pixel as the
  Windows build does; on the Steam Deck (offscreen, in a scratch folder) the Deck build
  finds the All-Stars ROM next to the dump, passes all its self-tests and draws the
  remastered picture; the Android app (emulator) finds both ROMs in its folder and shows
  the title, the PUSH BUTTON screen and play remastered. The whole 512-pixel widescreen
  canvas costs about 2.4 ms per frame on a laptop CPU and 4.2 ms on the Deck (the
  emulation itself 2.3 ms there), well inside a 60 Hz frame
* *Super Mario All-Stars + Super Mario World* (USA): the same pictures and music as the
  USA ROM, compared pixel for pixel at 4:3 and 16:9 in the 40 level runs, the castles'
  float runs, 8-4's water area, the path from 8-4's axe through the game's end, the name
  entry and the coin screen at one to four credits, the walk scenes and the castle at
  every flagpole (all but the intermissions of the four test runs that poke an area number
  their world does not have, which both draw as garbage); every All-Stars area set up in
  every world comes out the same
* All-Stars' music, against All-Stars itself run in a reference SNES emulator (LakeSnes,
  playing the ROM from power-on to 1-1): the sound unit's RAM after the port's uploads is
  All-Stars' byte for byte but for the program's variables; in 1-1 the ground theme's
  key-ons are All-Stars' note for note (120 compared: the same instrument and pitch on
  every voice, within 2.3 ms), and so are the death jingle's after Mario runs into the
  first Goomba; from one snapshot the sound unit's output is LakeSnes's bit for bit and
  blargg's snes_spc's to a correlation of 0.999; every song number is the one All-Stars'
  own code asks for (each call noted in the code); its level is the arcade music's (RMS
  about 1,390 in 1-1); the arcade's sound effects heard with the arcade's music muted;
  with the option off the sound is the same as before bit for bit; with it on the frame
  hashes are unchanged (seven scenes) and the self-tests pass (run-ahead included), on
  Windows and in the Linux and Deck builds; the same sound bit for bit on Linux as on
  Windows; the Android APKs build with it

Not yet verified: a full play-through to the ending by a person, a name entered for a
top score with the controls (the screen was checked with a score poked), and two-player
games beyond starting one. In the remastered graphics: a
coin heaven itself (the cloud lift was checked on a lift drawn as one), Toad's scenes
in worlds 2-7 (All-Stars' own code runs them, as it does world 1's, which was checked),
and a real window on the Deck in Game Mode. All-Stars' music has not been listened to
by a person yet (it was checked against All-Stars in the reference emulator, note by
note), nor run on the Deck or an Android device; it is mixed to mono, as the port's
sound is.

## Legal note

VS. Super Mario Bros., Super Mario Bros. and Super Mario All-Stars are trademarks and
copyrights of Nintendo. You need your own legally obtained dump of the board; the
project does not include, download or write one. Everything derived from your ROM (the
generated C, disassembly listings, builds, saves) stays on your machine. Do not
redistribute `generated/` or the built executable, since those contain the recompiled
game. The same holds for the All-Stars ROM of the remastered graphics and music: it is
read at run time from your file and kept in memory only; the repository holds no data
from it, only addresses in its program, the rules that combine its art and the song
numbers it uses. The SNES sound unit is emulated without its boot ROM (the program is
written into its memory directly).

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
executable with the player notes (`docs/PLAYERS.md`) into `dist/`. A build carries its
version (`git describe`: the newest `v*` tag, plus the commits after it) and the GitHub
repository of the checkout (its `origin` remote), which the Updates tab compares and
downloads from; `make VSSMB_VERSION=v1.2.0 VSSMB_REPO=owner/name` sets them where there
is no git.

### Linux and Steam Deck packages

`tools/linux/Dockerfile` is an Ubuntu 22.04 build environment with SDL2 built from
source as a static library that loads X11, Wayland, PipeWire, PulseAudio, ALSA and
udev at run time. The resulting binary needs only glibc 2.34+ and libm, so it runs on
SteamOS 3 and current distributions without installing anything; libcurl (for the
Updates tab) is loaded at run time when present. After `make recomp` (the container has
no git, so the version and repository come in from the environment):

```
docker build -t vssmb-linux tools/linux
export VSSMB_VERSION="$(git describe --tags --match 'v[0-9]*' --dirty)" VSSMB_REPO=owner/name
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e VSSMB_VERSION -e VSSMB_REPO -v "$PWD":/src -w /src vssmb-linux sh tools/linux/build.sh
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
port's own artwork; the phone edition differs only in its manifest. The app goes online
only for the Updates tab (its internet permission), and it may ask Android to install an
update of itself (the install-packages permission). It needs a platform 34+, build-tools,
an NDK and CMake from the SDK manager, plus a JDK; both APKs are signed with a key kept in
`~/.android/vssmb-release.keystore` (created on first use; keep it, an installed app can
only be updated with the same key). versionName is `git describe` (or `VSSMB_VERSION`),
versionCode the number of commits.

```
python android/build_apk.py                           # after make recomp
python android/build_apk.py --edition phone --install # one of them, and adb install it
```

Player notes for handhelds and phones: `docs/ANDROID.md`; device test tools (a virtual
controller for adb, frame-timing summary): `tools/android/`.

### Updates

**Check for updates** on the Updates tab asks the GitHub API for the newest release of
the repository the build came from (`releases/latest`: drafts and pre-releases are never
offered; nothing is contacted until the button is pressed) and compares its tag with the
build's version. **Update now** then downloads that release's package for the system,
over HTTPS only and only from the release's own downloads, and checks it: it must have the
size GitHub lists and the SHA-256 the release publishes for it (the digest GitHub keeps for
every uploaded file, and the release's `SHA256SUMS` file; where a release has both, both
must match, and a package without either is not installed). Then:

* **Windows**: the program is taken out of the zip in memory and written beside the
  running one as `vssmb.exe.new`; the running program is renamed to `vssmb.exe.old`
  (Windows allows renaming a running executable) and the new one takes its name.
* **Linux and the Steam Deck**: the same from the tar.gz; the running program keeps a
  second name, `vssmb.old` (a hard link, where the drive allows one), and the new file is
  renamed over `vssmb`, which replaces it in one step.
* **Android**: the APK of the app's edition goes to the system's package installer
  (a `PackageInstaller` session), which asks the player to confirm (the first time, it
  also asks to allow this app to install updates); Android then closes the game and
  installs the update over it, keeping its data, and the player opens it again.

On Windows and Linux the package's player notes go in with the program: `README.txt`
(`docs/PLAYERS.md`, which carries the licences of the code in the program, LakeSnes's among
them) and, in the Deck edition, `STEAMDECK.txt`. Each is written beside the old one as
`<name>.new` and renamed over it, before the program's own swap; if one cannot be replaced
(open in another program on Windows), nothing is installed. No other file of the package is
written. Settings, saves and the ROM are never touched. **Restart now** (Windows, Linux) shuts down
as Quit does and starts the new version; on Linux it takes over the same process, so on
a Steam Deck the game session carries on. `vssmb --update-check` prints what a check finds.

A release is a `v*` tag with the five packages attached (`make dist`,
`tools/linux/build.sh`, `android/build_apk.py`, built from that tag so they carry its
version) and their checksums as `SHA256SUMS` (`sha256sum vssmb-* > SHA256SUMS`).

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
frame, the game's own reaction time, measured; off while the All-Stars graphics are
drawn, whose work it would repeat) shows the game's response on the frame
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
  indirect jump. Immediate operands are constants in the C code, except the two
  marked `rom` in `symbols.txt` (the timer's tick, the one difference between the two
  known dumps), which are read from the loaded dump; a dump that matches the one the
  code came from apart from those runs the same native code.
* `runtime/` is the hardware: a dot-accurate PPU (sprite 0 hit drives the status-bar
  split), a cycle-stepped APU with the NES mixer and output filters, the Vs. System
  board (mapper 99: CHR bank from `$4016` bit 2, 2 KiB of work RAM at `$6000`, coin
  and service inputs and DIP switches on `$4016`/`$4017`, the coin counter on
  `$4020`), the RP2C04 palettes, and a 6502 interpreter that doubles as the fallback
  for untranslated code and as the reference model.
* `frontend/sdl_main.c` is the player: window, audio, input and the cabinet's
  controls, saves, states, rewind, shaders; `frontend/updater.c` is the Updates tab,
  over `frontend/http.c` (WinHTTP, libcurl loaded at run time, or Java on Android).
* the remastered graphics: `runtime/allstars.c` runs Super Mario All-Stars' own code
  (start-up, area setup, area parser, the intermission's and the title's drawing) in a
  sandbox SNES made of `runtime/cpu816.c` (a 65C816) and `runtime/snesppu.c` (a
  mode 0/1 line renderer with windows and colour math), in step with VS.'s parser, and
  draws each frame from that SNES state, VS.'s camera, block buffer, OAM and nametables.

Details are in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing

```
sh tests/get_test_roms.sh          # clones the public nestest/blargg collection (33 MB)
make headless                      # interpreter-only harness
make test                          # blargg suites through $6000 status reporting
make headless-recomp               # harness with the recompiled code linked in
python tests/cosim.py              # interpreter vs native, a 9,000-frame coin-op session
python tests/cosim.py --levels     # interpreter vs native in every world (about 3.5 minutes)
python tests/cosim.py --reset 3000,6000   # ... with the reset button pressed twice in the session
build/nes-headless-recomp --rom rom/*.nes --recomp --frames 9000 --ws-oam-check
                                   # the re-run engine must build the PPU's sprites exactly
sh tests/run_ui_bind_test.sh       # menu rebinding and text rows (synthetic events)
sh tests/run_updater_test.sh       # the updater offline: checksums, packages, notes, every refusal
build/vssmb rom/*.nes --rewind-test 1200   # whole rewind path: every re-run frame must match
build/vssmb rom/*.nes --chord-test         # the Start + Select rollback
build/vssmb rom/*.nes --reset-test 20000   # Play > Reset game after 20,000 frames: instant, game restarts
build/vssmb --update-check                 # what the Updates tab's check finds (online)
```

`vssmb --update-test VERSION [args...]` runs the whole update in the program's own
folder as if the build were that older version, then starts the installed program with
`args` (use it on a copy in a scratch folder).

The headless harness (`build/nes-headless --help`) scripts input
(`--keys "300:COIN;304:-;400:SELECT"`; `2A`, `2SELECT` ... for the second controller),
sets DIP switches (`--dip 07` = free play), writes memory before a frame (`--poke
"405:75F=3,760=0"`), presses the reset button before frames (`--reset 3000,6000`, each one
timed), dumps PNG screenshots every N frames, writes WAV audio, records
coverage (`--coverage FILE`), traces every instruction and dumps RAM. With `--allstars
ROM` it also writes the remastered picture (`--allstars-png FILE`, `--allstars-wide FILE`
for the widescreen canvas; numbered with `--png-every`) and can dump the SNES sandboxes
(`--allstars-dump PREFIX`: work RAM, VRAM, CGRAM, OAM, registers, the parsed columns).

## Repository layout

```
recomp/        the static recompiler (Python) and symbols.txt (address labels, no game data)
runtime/       hardware runtime + headless harness (C11), the MAME zip reader
frontend/      SDL2 player (menu, video, audio, input, touch controls, rewind, updates)
android/       Android app: CMake project, manifest, activity, build_apk.py
tests/         accuracy tests, cosimulation, nestest comparison, module tests
tools/         icon generator, Linux / Steam Deck build container (tools/linux), Steam
               shortcut script (tools/steamdeck), Android device test tools, DXGI adapter lister
docs/          architecture notes, player notes (Windows/Linux, Steam Deck, Android), latency notes
generated/     output of make recomp (git-ignored)
rom/           your dump (git-ignored)
```
