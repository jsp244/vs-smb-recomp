# Architecture

## Goals and constraints

1. The repository ships no game data and nothing derived from the ROM.
   Everything that comes out of the user's dump (the recompiled C, disassembly
   listings, the built executable) is generated locally and ignored by git.
2. The port must be *correct first*: anything the recompiler cannot translate
   must still run, and the translated code must be provably equivalent to a
   reference interpreter.
3. Ordinary tooling only: C11, GNU make, Python 3, SDL2.

Most of the runtime, the frontend and the recompiler is general NES code; what
is specific to this game is the Vs. System board, its palette, the flat 32 KiB
recompilation unit, the jump-table discovery, the cabinet controls and the
widescreen bands.

## Components

```
runtime/nes.h        shared state (cpu, ppu, apu, mapper, cart, sys) + inline ALU/memory helpers
runtime/cpu.c        6502 interpreter (fallback + reference model)
runtime/ppu.c        dot-based PPU, sprite evaluation, per-line scroll record for widescreen.c
runtime/apu.c        pulse x2, triangle, noise, DMC, frame counter, mixer, output filters
runtime/mapper.c     Vs. System (99); NROM, MMC1, MMC3/MMC6 for the test ROMs
runtime/mem.c        CPU bus: PPU/APU registers, controllers, Vs. coin/DIP inputs, OAM DMA, work RAM
runtime/cart.c       iNES / NES 2.0 loader, hashes, Vs. System detection
runtime/romzip.c     MAME's suprmrio.zip as an iNES image (zip directory, inflate, CRC checks)
runtime/palette.c    NTSC composite palette, the 2C03 and RP2C04 RGB palettes
runtime/widescreen.c the level beside the picture, from the nametables, with its objects (CPU sandbox)
runtime/bandwalk.c   the player walking into the left band (patches to the game's wall, scroll, sprites, collisions)
runtime/allstars.c   the remastered graphics: Super Mario All-Stars' code in a sandbox SNES, and the frame from it
runtime/cpu816.c     65C816 interpreter for that sandbox
runtime/snesppu.c    SNES line renderer (modes 0/1, priorities, windows, colour math, sprites) for it
runtime/spc700.c     the SNES sound unit (SPC700, timers, ports, S-DSP) for All-Stars' music: LakeSnes's (MIT)
runtime/sys.c        power/reset, frame loop, save states
runtime/headless.c   command-line harness (tests, scripting, pokes, screenshots, traces, coverage)
runtime/recomp_rt.h  helpers used by the generated code
recomp/recomp.py     the static recompiler
frontend/sdl_main.c  SDL2 player: menu, cabinet controls, scaling and shader passes, run-ahead, save slots, rewind
frontend/ui.c        the tabbed menu (choices, numbers, key capture, text rows)
frontend/rewind.c    rewind history: XOR deltas, run-length coded, in a byte ring
frontend/updater.c   the Updates tab: GitHub's newest release, package checks, installing in place
frontend/http.c      HTTPS GET for it: WinHTTP, libcurl loaded at run time, or Java on Android
```

Options that are not machine state (`ppu_no_sprite_limit`, the DIP switches in
`sys.vs_dip`, `widescreen_walk`) live outside the saved structs, so a save state or a rewind step
never changes a setting. Save states leave out the PPU frame buffer (and the
per-line scroll record after it), which every frame rewrites, and the APU's
sample ring; a state is about 24 KB.

## The Vs. System board

VS. Super Mario Bros. runs on Nintendo's Vs. UniSystem: the NES's 2A03 CPU and a
2C04 PPU on an arcade board. The game (MAME's `suprmrio`: PRG chips 1D 1C 1B 1A,
CHR chips 2B 2A) is 32 KiB of PRG and 16 KiB of CHR on mapper 99. Common old
iNES dumps are labelled mapper 3 with no Vs. flag, so `cart.c` recognises the dump
by its SHA-1 and sets up the board regardless; NES 2.0 headers (console type 1,
byte 13 = PPU type) and mapper 99 are honoured as well.

| address | the board |
|---|---|
| `$4016` write | bit 0 controller strobe; bit 2 selects the 8 KiB CHR bank (mapper 99) |
| `$4016` read | bit 0 serial data (left stick), bit 2 service button, bits 3-4 DIP 1-2, bits 5-6 coin slots 1-2, bit 7 0 |
| `$4017` read | bit 0 serial data (right stick), bits 2-7 DIP 3-8 |
| `$4020` write | bit 0 coin counter (one pulse per coin) |
| `$6000-$7FFF` | 2 KiB of work RAM, mirrored |

What the game does with them (from its code):

* It reads the DIP switches once, at reset (`$9274`), reverses their bit order and
  keeps them at `$6602-$6606`: coinage = 4 * sw1 + 2 * sw2 + sw3 (tables at `$9314` /
  `$931C`: 1c/1c, 2c/1c, 3c/1c, 1c/2c, 1c/3c, 1c/4c, 1c/5c, and 7 = free play, which
  forces 32 credits and shows FREE PLAY), sw4 = two lives instead of three, 2 * sw5 + sw6
  = 100 / 150 / 200 / 250 coins for an extra life (`$BAF2`), sw7 = the fast timer
  (`$0787` reload 17 instead of 20, other start values), sw8 = three lives after a continue
  instead of four.
* A coin (`$9389`) counts when its switch opens again within 15 frames of closing;
  held longer it is marked jammed and refused. The frontend therefore turns one press of
  Insert coin into a closure of exactly four frames.
* The start buttons are the Select lines of the two controllers. In a one-player game
  the game ORs both controllers; in a two-player game it reads the current player's
  (`$077A` players - 1, `$0753` current player), which the frontend uses to hand one pad
  to whoever's turn it is.
* The level data lives in CHR ROM bank 1: with `$4016` bit 2 set the game reads it
  through `$2007` into the work RAM (`$AB7F`: enemies to `$6000`, area objects to
  `$6100` until `$FF`, more to `$6200` until `$FD`), and the default top score from
  `$1EBA`. So the CHR bank switch must take effect on the PPU's data port immediately.

**Palette.** The 2C04 PPUs output RGB through a scrambled palette ROM (a copy
protection): all four share one master palette in different orders. `palette.c`
holds the master palette (3-bit DAC levels per channel), the four scrambles, and
the 2C03's plain table, from the NESdev wiki; MAME's `rp2c04-0004.pal` (raw DAC
levels) agrees on all 64 entries. Against this game: its four level palettes
(`$8EA5..`) give, through RP2C04-0004, exactly the RGB values of the NES version's
palettes through the 2C03 (128 of 128 entries). Emphasis on the RGB PPUs turns a
channel fully on instead of darkening the others. The NES (composite) option maps
each index through the scramble to the master palette's index, which is the 2C02's
for its first 13 columns, and takes the nearest 2C02 colour for the extra greys.

## Timing model

The CPU is the master clock. Each instruction adds its base cycle count up
front (page-crossing penalties are added when the dummy read happens) and then
calls `sys_sync()`, which runs the PPU to `cpu.cycles * 3` dots and the APU to
`cpu.cycles`, then polls interrupts. Register accesses (`$2000-$2007`,
`$4000-$4017`) catch the PPU/APU up before they execute, so a read of `$2002`
sees the flag state at the end of the instruction, which is when the real bus
read happens. The game's status bar split waits for the sprite 0 hit flag, which
the dot-based PPU raises on the exact pixel.

Interrupts: NMI is edge-latched by the PPU (vblank flag AND enable), IRQ is a
level mask with three sources (mapper, APU frame, DMC). An edge or assertion
that lands on the last cycle of an instruction is only seen by the next one,
and CLI/SEI/PLP poll with the *previous* I flag, which is what makes blargg's
`cli_latency` pass. The `$2002` read race with the vblank flag is modelled.

The frame loop in `sys.c` runs instructions until the PPU reports the start of
scanline 240. In native mode `sys_sync()` also returns nonzero at that point so
the generated routine returns to the dispatcher at the same instruction
boundary the interpreter would stop at; this keeps the per-frame state hashes
comparable.

## Recompiler

### Discovery

* With 32 KiB of PRG the board has no bank switching, so the whole of
  `$8000-$FFFF` is one unit (`--layout flat`, chosen automatically): one entry
  table of 32768 slots, routines may span the old 8 KiB boundaries, and stores
  to ROM need no dispatcher exit. (The 8 KiB "mmc3" layout is still there for
  other boards.)
* Recursive descent from the reset/NMI/IRQ vectors follows fallthrough, branches,
  JMP abs and the return path of JSR, except after a JSR to a routine labelled
  `JumpEngine...` in `recomp/symbols.txt`: SMB's dispatcher pops its return
  address and jumps through the word table that follows the JSR, entry A. There
  the words are taken as code pointers until one does not point at an official
  instruction (a wrong guess only adds a routine that is never entered). This
  takes static discovery from 1,209 to 12,156 instructions.
* Coverage files (`--coverage`) are one byte per PRG byte, bit *k* set when the
  byte started an instruction while mapped in slot *k* (`$8000 + k*$2000`).
  The headless harness writes them, and in native mode it records only the
  instructions that fell back to the interpreter, so the loop converges. With the
  jump tables followed, no session played so far has needed one.

### Emission

* One C file per unit; one `static void` function per routine. A routine is a
  connected component of the intra-unit control-flow graph (fallthrough,
  branches, JMP abs), capped at `--max-routine` instructions so compilers stay fast.
* Every instruction is `case 0xADDR: L_ADDR:` inside the routine's `switch
  (cpu.pc)`, followed by the translation and `SYNC(next)`. Branches and in-
  routine JMPs become `goto`; everything else that leaves the routine (JSR,
  RTS, RTI, JMP (ind), BRK, cross-routine jumps, interrupts, frame end) sets
  `cpu.pc` and returns to the dispatcher.
* The emitted operand code mirrors the interpreter: dummy reads on indexed page
  crossings, the double write of read-modify-write instructions, zero-page
  wraparound, the JMP (ind) page bug, open-bus-free fast paths when an operand
  range is provably RAM.
* `recomp_run()` looks up `cpu.pc` in the unit's entry table and calls the
  routine; a miss (code from RAM, an untranslated address, a jammed CPU) returns
  0 and the frame loop executes one interpreted instruction instead.

### Cosimulation

`tests/cosim.py` runs the same key script through `nes-headless` (interpreter)
and `nes-headless-recomp --recomp` and compares a per-frame FNV-1a hash of CPU
registers + cycle count, RAM, work RAM, framebuffer, OAM, palettes, nametables
and bank registers. The default session is a coin-op one (three coins, one held
too long, 1P start, running and jumping through deaths, game over and the
continue screen); `--levels` starts every world's areas 0-4 by poking the world
and area number (`$075F`, `$075C`, `$0760`) after the start and letting the first
death reload the level; `--walk N` walks into the widescreen band with the player
allowed N pixels beyond the screen's left edge (a death and a stomp there, the
area's start, the page before a halfway restart). On a mismatch, build the tracing variant
(`make BUILD=build-trace RECOMP_DEFS=-DRECOMP_TRACE headless-recomp`) and diff the
per-instruction traces (`--trace FILE --trace-start FRAME [--watch ADDR]`).

## Widescreen

`runtime/widescreen.c` draws the level beside the picture on a 512 x 240 canvas
(up to 128 pixels on each side). SMB keeps a 512-pixel window of the level in its
two nametables (vertical mirroring): the area parser renders two metatile columns
for every 32 pixels the screen scrolls and runs 90-128 pixels ahead of the right
edge, and what scrolled out on the left stays until the parser comes round again
(measured from per-frame RAM and VRAM dumps). The PPU records where each line's
background starts (v and fine x after the previous line's horizontal copy, and
`$2000`), so the bands are those nametable columns drawn with each line's own
scroll: the status bar's lines show the blank tiles beside it, the playfield the
level. The parser's frontier (`$0725`/`$0726`) and the screen's left edge
(`$071A`/`$071C`) bound what is valid (16 pixels kept off the column being
rendered and the oldest one; nothing before the start of the area, unless the area's
parser went through a castle loop, which takes four pages off every page number: the
columns before the start are then the level's from before the loop, and the page
arithmetic wraps, the screen's page going to $FF while the parser's is past 0), and a frame
whose playfield scroll does not agree with the screen position is not a level
(intermission, text screens), so its bands are black. Objects do not exist beyond
the screen until the game spawns them at its edge, so the bands are background
only. `nes-headless --widescreen FILE` writes the canvas and checks that its centre
is the PPU's frame and that the centre re-rendered from the nametables differs from
it only by the sprites (0.2% of the pixels in 1-1). The frontend shows the part a
16:9 or 16:10 picture needs at the 4:3 pixel shape (341 or 307 columns, 319 / 287
with the overscan crop), or with Integer scaling in fullscreen every column the
screen holds at the multiple.

**Hooks and the CPU sandbox.** `recomp/symbols.txt` marks two instructions
`hook`: GameEngine's first ($ADAB, the frame's object code) and InitializeArea's
($965C). The generated code calls `HOOK(addr)` there and the interpreter checks
`sys.hook_pc[]` before each instruction, so either path reports them; the hook
(`engine_hook`) only copies RAM, work RAM and the CPU registers, and cosimulation
is unaffected. `cpu_sandbox_run` (cpu.c) runs game code from such a copy with the
interpreter on private RAM and work RAM: `sys.sandbox` makes `cpu_step` skip
`sys_sync` (no PPU or APU time passes), drops every hardware write (the enemy
loader writes the Vs. CHR bank to $4016), and ends the run at any hardware read
except the PPU data port, which reads CHR ROM through a latch and CHR bank of the
sandbox's own (mem.c) because VS. Super Mario Bros. loads its level data out of
CHR ROM through $2006/$2007 when an area starts.

**Objects beside the picture.** The engine copy whose sprites the PPU shows is
the previous frame's (GameEngine runs after the sprite-0 split and its OAM is
DMA'd at the next NMI). It runs again from GameEngine to $AE14 (past the last
sprite update, before the area parser) with ScreenLeft/ScreenRight ($071A-$071D)
moved 112 pixels towards a band; the sprites that differ from the PPU's OAM are
the ones the camera placed, minus any that wrapped round the moved screen's 8-bit
X. The player's own eight (by Player_SprDataOffset, $06E4, not by their tiles)
are the picture's wherever the PPU shows them, as the game never hides him at
the sides: the repeat does not redo his frame (his bounding box is made before
GameEngine with the real camera, every other object's in it with the moved one,
112 pixels apart, so no lift holds him there and he falls in another pose, and
an enemy 112 pixels away seems to touch him), and past the moved screen's edge
his 8-bit X puts him 256 pixels off. Of his sprites the band takes only those
the real frame hides beside the screen (bandwalk.c), where his X has not
wrapped. The sprites the band takes are drawn into the band and into the 8-pixel
columns at the picture's edges where the PPU's frame shows background, with
sprite priority and behind-background rules. With the camera left where it was,
the repeat builds the PPU's OAM byte for byte
(`nes-headless --ws-oam-check`: 62,688 frames over the 40 level runs, no
difference); a repeat costs 0.01-0.03 ms.

**The page before a part-way start.** InitializeArea takes its start page from
the halfway page or a pipe's entrance page and sets the parser up to render 12
column sets from there, so after a mid-area start the nametable columns left of
the screen hold the previous screen's leftovers. `make_fill` runs the area-start
copy of InitializeArea in the sandbox to its `STA ScreenLeft_PageLoc` ($9681),
puts the start page minus one in A, finishes it, then calls AreaParserTaskHandler
($A18B) 64 times (eight 32-pixel column sets with their attributes), applying the
nametable writes it queues in VRAM_Buffer2 ($0341, the NMI's format) to a copy of
the nametables. Band pixels left of the start page come from that copy while the
area on screen (world, level, area number, type and data offset) is the one it was
made for. The remastered graphics draw that page in their band at the same moments
(`widescreen_fill_page` tells them so, with the start page and a new number for each
area start), with the area they draw in place of RAM's numbers, which VS. moves on to
the next area's while a warp zone's pipe or a level's end still shows the old one
(see "Jumps of the machine" below). All-Stars' parser parses its 16 columns of VS.'s
data once per area start, in a sandbox of its own set up as for the area but a page
earlier, and they are decorated as every column is, the start page's first columns on
their right. In layer 1 they take the slots of the page after the start page until
VS.'s parser renders that page's columns there, which it does only once the band has
left them behind.

**Walking into the band.** The game stops the player at its screen's left edge
(ChkPOffscr puts him back there when his horizontal offscreen bits say he is
beyond it), and everything that places him works in 8-bit screen coordinates,
which wrap round to the right edge once he is left of it. Instructions marked
`patch` in `recomp/symbols.txt` call `sys.patch` just before they run: the
generated code through `PATCH(addr)`, the interpreter and the sandbox through
`sys_patch_map` (a bit per address). Unlike a hook a patch may change registers,
flags and memory, so it may depend only on the machine and the settings.
`runtime/bandwalk.c` installs thirty-four, which change nothing while
`widescreen_walk` is 0; the frontend sets it every frame to the band its view
shows on the left (whole columns where Integer filling the screen cuts the
outermost one), and `nes-headless --ws-walk N` sets it for tests.

| patched | what changes |
|---|---|
| `$AEB2` ChkPOffscr | the offscreen bits read "on the screen" while he is within the band; at its edge he is stopped as KeepOnscr stops him at the screen's (position, and speed unless right is held). The edge is never beyond what the band shows of the level (`widescreen_level_left`: the parser's frontier less 512 plus the 16-pixel margin, the area's first pixel, or the page drawn before a part-way start) nor 104 pixels (the band's sprites come from the engine re-run 112 pixels out); standing on a platform keeps the screen's edge |
| `$AE52` ScrollHandler | the scroll test gets his real position instead of last frame's wrapped 8-bit one |
| `$ADC6` GameEngine | his sprite columns that wrapped to the right edge are hidden; the left band's re-run draws him |
| `$B007` PlayerSubs | his bounding box is clipped at the screen's left edge, as CheckLeftScreenBBox clips every other object's |
| `$D7B7` PlayerEnemyCollision, `$D7F4` HandlePECollisions | an enemy or power-up the game skips as not wholly on the screen is let through while it is beside the screen where the band shows it, and tested every other frame as one on the screen is: the contact as below, and with none the game clears its touching bit (Enemy_CollisionBits d0, set at a contact so that it counts once) itself. Letting it through only while the boxes touched had left that bit set once they parted, so nothing more happened between them: a shell stomped in the band could not be kicked there |
| `$D7C8` / `$D7CB` around PlayerCollisionCore | both boxes are moved for the test into an 8-bit frame where neither wraps and put back after it, so the game's own rules decide (stomp, injury, power-up) |
| `$D978`, `$D4E8` SetupFloateyNumber | a score for something beside the screen is shown at its left edge |
| `$D734`, `$DACA`, `$DAF6` | a hammer or platform does not touch him while he is wholly beside the screen (his box is then the game's empty one at the edge) |
| `$9687` InitializeArea | at a part-way start (the start page in A) the page before it gets its collision: InitializeArea cleared the block buffer ($0500-$069F, 13 rows of 16 metatiles per page parity) and the game renders from the start page on, so `widescreen_fill_collision` copies the last 128 pixels of the band's rendering of that page (`make_fill`'s block buffer) into the game's. Those slots belong to the next page's last columns only once the parser is 512 pixels past them, out of reach. Without that copy (the walk switched on later in the area) the wall stays at the start page |
| `$B593` FireballObjCore | a fireball beside the screen's left edge, its box still within the band, has only its vertical offscreen bits counted, so it flies and bounces on there (it still goes below the screen, and once it has left the band); the game's own bits include the screen's first 8 pixels, so without this one thrown there went at once |
| `$B51B` ProcFireball_Bubble | a fireball's sprite (or its explosion's four) that wrapped to the right edge is hidden; the band draws it |
| `$D66A` FireballEnemyCollision, `$D9A5` / `$D9C6` EnemiesCollision | an enemy the game skips as not wholly on the screen is let through while it is in the band (not one partly above or below the screen) |
| `$D673` / `$D676`, `$D9CF` / `$D9D2` around SprObjectCollisionCore | both boxes are moved for the test as for the player's: a fireball hits an enemy in the band, enemies there turn at each other, and a kicked shell knocks them over |
| `$B83C` VineObjectHandler, `$B878` / `$B87C` | a vine in the band is kept: SMB's offscreen test erases it, and VS. adds a check of its own after it ($B86C) that erases one left of the screen or within 9 pixels of its edge, with its climbing metatiles; its sprites that wrapped are hidden |
| `$DE13` PutPlayerOnVine | a vine grabbed at an even page's first column (block buffer address low byte 0) gives him the vine's page, not the screen's right edge's, while it is beside the screen |
| `$ADD9` GameEngine | a coin knocked out of a block there (and its score) and a brick's chunks: the sprites JCoinGfxHandler and DrawBrickChunks wrapped to the right edge are hidden; the band draws them |
| `$CC87` ProcFirebar, `$CCC8` | a firebar whose centre is off the screen's left side is processed while its parts can reach the band (ProcFirebar skips it otherwise); its centre's sprite is hidden in the real frame where it wrapped |
| `$CD29` / `$CD49` DrawFirebar_Collision | a part is shown or hidden by its true place (within the band or on the screen), not by its 8-bit distance from the centre, which hid the parts reaching across the screen's edge; a part's sprite beside the screen is hidden in the real frame (its Y in $07 kept for the test) |
| `$CD87` / `$CD94` FirebarCollision | a part beside the screen is tested (the game skips one at X $F0 or more), and the player's sprite 1 X against the part's by their true places: the 8-bit difference let a part 256 pixels to his right hurt him while he stood in the band |

The screen still never scrolls back. `tests/cosim.py --walk N` (a death and a stomp in the
band, the area's start, a halfway restart, a Koopa stomped in the band and its shell kicked
out through the picture and out through the band and into a Goomba there, fireballs thrown
both ways in the band and one hitting a Goomba there, a vine scrolled into the band and
climbed there, a brick broken there, and two firebars: one 256 pixels to his right that
must not touch him, one in the band that does) stays bit-identical with the patches working, and the band's repeat of the engine still builds the PPU's
sprites exactly (`--ws-oam-check`); with `widescreen_walk` 0 the standard session
and the 40 level runs are unchanged.

## Remastered graphics (Super Mario All-Stars)

`runtime/allstars.c` draws the frame as Super Mario All-Stars draws Super Mario
Bros., from the player's own All-Stars ROM (hash-checked; USA, and the Wii and
Switch Online copies whose banks $03-$0C are the same). All-Stars' SMB1 is the NES
engine rewritten in 65816 code: most of the RAM layout and the object and metatile
logic are the NES game's, the art and the PPU setup are new. Rather than describing
that art, the port runs All-Stars' own code in a sandbox SNES: `cpu816.c` (the whole
65C816 instruction set, both register widths, decimal mode) on 128 KiB of work RAM,
with a bus that keeps VRAM, CGRAM, OAM, DMA and HDMA, the multiplier and the PPU
registers in a `snesppu_t`. Nothing in the sandbox touches the NES machine, and the
NES machine runs the same with it or without it (per-frame state hashes compared).

**Areas.** At start-up the sandbox runs All-Stars' reset code to its first NMI wait
(base tiles, registers), and keeps that state. When VS. starts an area (its
InitializeArea hook at $965C), the sandbox copies that state and runs All-Stars'
InitializeArea ($03:9E0C) and area graphics and colour setup ($04:956B, $04:9A88) for
the All-Stars area that looks most like VS.'s, then All-Stars' NMI to upload what they
queued. VS. numbers its areas differently and has levels of its own, so `map_areas`
scores every All-Stars area of the same type: first the header's background (night,
snow) and style (mushrooms, cannons), because All-Stars picks the sky and scenery by
area number, then the longest common subsequence of the object lists. The header's
settings themselves are VS.'s, decoded from the area's two header bytes as its
GetAreaDataAddrs ($AC0E) does: VS. copies each area's data to PRG RAM ($6200), where the
header stays while the area runs (objects change the settings in RAM as the parser meets
them, so a later rebuild cannot take them from there). One area leans on the one before
it: All-Stars gives its 8-4 water area a castle's style ($BA 3), and its setup loads the
water's tiles and leaves the rest of VRAM as the castle the pipe comes from had it (the
castle's stone, which its wall and floor are drawn with). So before an area of a castle's
style that is no castle, the sandbox sets up the level's first area (All-Stars'
LoadAreaPointer lookup, $04:C01F, by world and area number) and keeps its VRAM. The
water's tiles it loads are the castle's own (its area number's tilesets, $04:97CD): BG
tiles $100-$17F, where a water area has its seabed and coral, hold the ruins its layer 2
is drawn with, and All-Stars' 8-4 has a wall where VS.'s has a coral, above the pipe out.
So for a water area in a castle's style the sandbox first sets up All-Stars' first water
area and keeps those tiles, which go to $300-$37F (VRAM nothing else uses), and layer 1
takes the tiles of $100-$17F from there.

**Columns.** VS.'s AreaParserCore ($A2D7) is a hook too: for every column VS. parses,
All-Stars' AreaParserCore ($03:A436) parses the same column of VS.'s own level data
(copied from VS.'s work RAM and converted to All-Stars' format, where row-15 objects
have a third byte), in step: page, column and block-buffer position are VS.'s. The
metatiles that come out are All-Stars' choice for VS.'s level (the bushes a row lower,
bridge rails with ends, two castle wall patterns). One object is moved: All-Stars' tall
castle (each x-3 ends with one) is ten columns wide ($04:8F44), towers at both ends and
three doors between, where the NES's is five with a door in its first column, in which VS.
stops the player and puts him behind the background; so All-Stars' starts two columns
before VS.'s, its first door where the arcade's is (and its star flag's column the
arcade's). Run-ahead and rewind re-parse
columns: a ring of 64 snapshots of All-Stars' parser state (the low 8 KiB of work
RAM) lets a column be parsed again from the state before it; when none is kept, or the
area is new or not the one built, the area is rebuilt and replayed from its start at
the next picture (VS.'s parser run again in the CPU sandbox, column by column, with
All-Stars' beside it). VS.'s demo, for one, carries on from the title's columns. The
replay takes the area from VS.'s RAM (its data in PRG RAM, the data pointers, the area
type, the header) rather than from InitializeArea's AreaPointer, which the title screen
leaves pointing at another area, and starts on the page VS.'s parser began the area on
(seen at its first column: the halfway page after a death is cleared later), so that it
gives VS.'s own metatiles for every column; a debug self-check found the replayed columns
equal to VS.'s block buffer in all 32 levels, deaths and restarts included. A castle's
loop (VS.'s ExecGameLoopback at $BFA5, when Mario takes the wrong path) sends the parser
four pages back to the loop's place in the area data, and the columns parsed before it
stay in the nametables (four pages are two nametables): All-Stars' parser takes the same
step, as its own loop code ($03:C3F5) does (the object page four back, the page select
cleared, VS.'s new data offset in All-Stars' copy of the data), and the columns go on
from there. A replay less than two pages past a loop's new page goes through that loop
too (at its page, VS.'s loop table: $BF84, $BF8F, the offsets in PRG RAM at $6000) if
its columns then match VS.'s block buffer better than without it.

**Jumps of the machine.** Run-ahead's own return to the frame it started from is
followed by the snapshots above; every other jump (a slot load, each rewind or forward
step, Start + Select's rollback, the game-shot test's state) is reported with
`allstars_reset`, and the area is rebuilt from the loaded state at the next picture.
Which area that is comes from the areas started before (their numbers, the page their
parser began on and the frame, kept as each first column is parsed): the latest one
before the loaded frame with the same numbers, or with the area data PRG RAM holds,
since VS. sets the next area's numbers while the old one is still on the screen (a warp
zone's pipe sets the world as Mario goes down, 49 frames before the screen goes black;
the end of a level or a world its area number and type). The same holds without a jump:
until VS.'s InitializeArea copies the next area's data to PRG RAM, the area built is the
one shown, so the warp zone stays in All-Stars' art down to the black screen. A rebuild
replays to the column the parser renders next, which while a column's tasks run is the
one after RAM's (AreaParserTaskNum, $071F), and past a castle loop seen in that visit it
takes up layer 2's seam as the loop left it (or stops at the loop, when the parser has
not rendered past it yet, and lets parser_column take the loop's step again). Its
sandboxed InitializeArea reads the area's data through the PPU data port stepping by
one, as the game does when it starts an area (between a column's writes the PPU steps by
32). A jump to a moment of the same visit at which the parser renders the same column,
with no castle loop in between (holding rewind while standing, or between two columns),
keeps the columns as they are, so that holding rewind costs about what drawing does. The
areas started and the loops seen after a jump's frame, but recorded before the jump, are
of the timeline the machine left (a rewind to before a death or a castle's wrong path,
and the game going on otherwise): fast-forward can still take that stretch again until
the game has run on for the frontend's undo window (1.5 s), after which they are
forgotten, so that a later rebuild takes no start page or layer 2 seam this timeline
never had. A jump also leaves widescreen.c's own record of the area (its copy of the
area's start, which the band's page before is made from, and its flag of a castle loop
since) as the frames it last ran had it, until VS. starts another area: in between, the
remastered band shows the page before and blacks the band before the area's start by the
visit built (its start page) and the castle loops seen in it up to the moment loaded,
keeping the page it made in that visit. The arcade's band keeps its own record. A jump
that lands between InitializeArea and the area's first column (the WORLD screen after a
death, a pipe's black frames) leaves that start unrecorded, since its hook ran before the
jump: the first column parsed at the screen's left edge, where VS.'s parser is only as an
area starts, is then taken as the start, unless it is recorded already in this run. A
power-on (a game started from the launcher, Stop, new DIP switches) counts the frames from
0 again: the records stay, for a state saved before it, but go by their run's number
(`allstars_power`), so that no start or castle loop of one run passes for another's.

**All-Stars' own decorations.** All-Stars' own levels carry row-15 objects the NES format
has no form for (three bytes; its own kinds, dispatched by $04:8E15 from a table at
$04:8E43, each writing a few metatiles of its own): $32 a castle's first staircase as
smooth steps, $36/$38 a floor's ends at a gap, $3A/$3C the steps between floors of two
heights, $34 caps on the wall's brick halves where the wall stops and on the blocks that
carry a firebar, $3E coral on the water levels' terrain, $12 a tree crown erased that a
block left standing. VS.'s data has none of them, but All-Stars placed them wherever the
level's shape asks for them (in its six castles every floor end at a gap has its end
tiles but one), so the port derives them from the columns themselves: each parsed column
is kept plain, and once the three after it are parsed it is redrawn from the plain
columns three either side of it by rules that do what those objects do there, with the
objects' metatiles read from their handlers. On VS.'s levels that are SMB's, the rules
give All-Stars' own tiles cell for cell but in a few cells that are All-Stars' own slips
(the last step of 8-4's castle undecorated, an end a column off over lava); they never
change a cell between solid and empty (by All-Stars' own bounds, $03:A5C8). The one
object All-Stars placed by hand for another reason, a wall ($28) closing the corridor
behind 8-4's exit pipe, is a collision change and is left out (VS.'s data closes it
already). Where VS.'s levels take shapes All-Stars' have not, the rules keep to what it
does: no end under a block or a step standing on a floor (the rounded corner would not
meet the square one above), a lone column between gaps left plain but for its end at the
bridge, and the end wall of a water level coral right through (All-Stars leaves its
inside plain where its picture never reaches; the widescreen bands do). In world 8
All-Stars' parser draws the water levels' terrain as the castle's wall halves, so 8-4's
water area gets the castle's caps. One look of All-Stars' own comes from ordinary
objects: over the floors 5-4 and 7-4 leave hanging in mid-air its data lays rows and
columns of solid blocks, so those floors are drawn as blocks; it is the only change of
look, not of shape, between its levels and SMB's besides the row-15 objects. The pairs
the fallback mapping learns come from the plain columns.

**A frame.** Layer 1 is the parsed columns as All-Stars' map16 words, with the blocks
VS. changes during play (its block buffer) put in as All-Stars' own code writes them:
the place of a bouncing block ($27) and the used block ($FC) from its PlayerHeadCollision
($03:BEC6), the vine's cells ($2A, $03:BA99), a coin or brick gone as nothing, and
otherwise by the pairs the two parsers produced. Bowser's bridge VS. takes away in its
nametable only: after the axe its BridgeCollapse ($CF2F, VictoryMode's first task; VS.'s
VictoryMode is OperMode 3) blanks the axe, the chain and the 13 pieces, one every 4
frames, at the addresses of its BridgeCollapseData ($CF20, $22xx), counted by
BridgeCollapseOffset ($0369), and its block buffer keeps the bridge. So each of those
cells that VS.'s parser filled with that object is drawn empty from the frame the
nametable shows it blank, as All-Stars' own collapse ($03:D4D0) writes its blank there,
and each piece leaves All-Stars' rubble falling for 27 frames ($03:D4A0, drawn by its
Bowser routine at $05:CA13), aged from the offset, Bowser's feet counter ($0364) and,
after the last piece, how long he has been falling. From BridgeCollapse's first step (the
axe's cell, up to 32 frames after the axe) to the end of his fall Bowser roars as All-Stars'
does ($03:D53C, $05:D060: its frames 6 and 5 by turns every 4 frames), on the same count;
once his falling speed is held, how far he has fallen below the height his bounding box
keeps from the bridge gives it. Layer 2 is All-Stars' hills from its
RAM pages at half the camera's speed; layer 3 its clouds at a quarter above ground (under
water the surface's waves, elsewhere nothing), below the status bar's IRQ split, and
above it the status bar from VS.'s nametable in All-Stars' font. At night ($0ED1) the
split is layer 2's instead, as All-Stars' NMI and IRQ ($03:84A8, $03:851A) have it: the
clouds' scroll above the line, the hills' below, and layer 3 is blank. After a castle's
loop the camera is four pages back and layer 2's scroll two of the layer's pages; All-Stars
draws that layer in strips just ahead of its scroll ($03:A2BA, $04:9B35), so what it drew
before the loop stays until new strips replace it, and the port draws layer 2 the same
way, from before the loop left of the seam the strips reached. The seam takes effect on
the loop's first picture: ExecGameLoopback runs inside GameEngine's enemy loop, so the next
frame's GameEngine hook sees VS.'s parser 64 columns behind the last one rendered, at a
loop's first column, and notes the loop there (at the picture it first showed, which a
rewind or state load to a later moment finds), a few frames before the parser renders that
column; the picture in between takes the same test. Waiting for the column had shown layer
2's new place for those frames, then the old one again: the castles' background
flickering at each loop. The sky's gradient is the sandbox's HDMA, replayed line by line; the
animated tiles (? blocks, coins, bushes, water, lava) are All-Stars' frames at VS.'s
frame counter. Sprites come from VS.'s OAM, attributed to their owners by VS.'s
sprite-data offsets (the copy of RAM the engine built them from, with one shuffler
step undone when that copy is a frame ahead; the bands' sprites, which that copy's own
repeat built, by its offsets as they are): the player's 8 sprites become
All-Stars' frame for the same pose (8 tiles per pose in bank $0A, Luigi's $4000
further) in All-Stars' colours (Mario, Luigi, fiery, the star's 4-set cycle); enemies
keep the NES tile numbers, which All-Stars mostly kept, with the tiles it moved found
by comparing the two enemy graphics tables (upside down, VS.'s rows swapped back
first), and get All-Stars' palette for their type; fireballs, fire bars, explosions,
jumping coins, hammers, bubbles, lifts and bouncing blocks get the tiles, palettes and
16x16 sizes All-Stars' own handlers use (the constants are noted at each in the code).
The lifts that move sideways are in front of all of layer 1: All-Stars draws lifts
behind its lava (whose tiles have the priority bit) and has 8-4's lava lift a row
higher than SMB's, while VS.'s runs on the lava's top row; a lift that moves up or down
(a falling one, the player on it) sinks behind the lava as the player does.
The objects All-Stars draws with routines of its own instead of its table are drawn
as those routines do, placed from any of the object's arcade sprites: the goal flag,
the castle's star flag, Bowser (one figure for both halves), his flame and its sparks,
Bullet Bills, Podoboos, shells still and kicked, the vine, the coin heavens' cloud
lift and fireworks; the floating score numbers are All-Stars' two tiles for each value
($03:8B0C: an 8x8 digit and a 16x16 half for 1,000 and up, in front of everything).
VS. draws a defeated enemy's points in the first two of its own sprites
(FloateyNumbersRoutine, $86E1), so a pair of score tiles there is taken for points by its
tiles, also on the frame that sets them, which the copy of RAM from before GameEngine
does not know, and never gives the object's place: a stomped or kicked shell stays on the
ground under its points instead of following them up. At a castle's end All-Stars' own retainer code runs in a copy of
the area's sandbox once a frame (Toad tied up in a sack that wobbles, then its scene
for each world), with VS.'s Toad, camera and frame counter, and its sprites are drawn.
The princess is All-Stars' standing pose mirrored to face the player as the arcade's
does (All-Stars' own scene has her in a cage, facing away until she turns), her feet
where the arcade's are; at the game's end the seven Toads VS. adds around them ($8552,
the scene of The Lost Levels, which All-Stars has as well: $0D:9C2B, the same tables)
are All-Stars' Toad standing free, at the arcade's places and left out in its turns.
VictoryMode runs no GameEngine, so there the owners come from RAM, a shuffler step
ahead; $8552 puts the princess's sprite data offset back after the shuffler has moved
it, so in world 8 her sprites are also found by her place, and the Toads by theirs
(its tables). `snesppu.c` then draws mode 1 with
All-Stars' priorities, windows and colour math (layers 2 and 3 on the subscreen,
added to the backdrop; under water instead the fixed colour's gradient, added to
layers 1 and 2, the sprites of palettes 4-7 and the backdrop inside window 2: all 256
columns below the surface). In the widescreen canvas the frame is drawn in three
parts, the bands with the sprites the game builds there (`widescreen_band_oam`: the
same engine re-run as the arcade bands) and the windows of the picture's nearest
column, and beyond the level the game holds the canvas is black, as with the arcade
graphics. The picture also gets, behind its own sprites, those of the bands' sprites
that reach its edge columns where the game hides them (an object straddling an edge,
the player walking into the left band), as the arcade's canvas draws them into its 8
edge columns, so that both cross the edges whole; a special object the picture draws
itself is left out of them.

**Screens.** A second sandbox draws the rest. The intermission is All-Stars' own: its
player head and world-picture objects, its world picture, its text and lives, the
routine that packs each sprite's size into the OAM's high table ($05:DD7A, which its
frame code calls; Bowser in the castles' picture is 16x16 sprites), then its NMI. The title's banner is All-Stars' (its DrawTitleScreen's stripes, uploaded by its
NMI) a row higher, its SUPER six columns right, and "VS." in front: All-Stars' S, the
dot of BROS. with a stroke's shadow under it, and a V. All-Stars has no V; the arcade's
V is its U with a narrowed lower half, so the port does the same to All-Stars' U: each
pixel of the arcade V's lower half is painted as All-Stars paints a straight stroke at
that height (outline, fill, the red band, the shadow), chosen by its distance from
the letter's edge; the same rule turns the arcade's U into All-Stars' U but for 6 of its
256 lower pixels. The banner and the title's text scroll away with the demo, column by
column, as the arcade's nametable loses them. Text in a level (the title's lines, the
castles' messages) is VS.'s nametable text in All-Stars' layer 1 font, left out where the
colours its glyph's pixels have are the backdrop's: at the game's end VS. turns the
backdrop to its messages' colour, all but THANK YOU MARIO!'s ($84CF). The screens only
the arcade has (PUSH BUTTON, the continue countdown, game over, the coin and credit
lines, the attract's high scores, the name entry) are VS.'s text in All-Stars' layer 3
font on the intermission's backdrop, lines the arcade blinks blinking with it; white
text keeps the font's colours, other colours (the top three scores, the name entry's
lines) get the font's shades of theirs in layer 3's palettes 4-7. The rest of the
nametable that is not text (the cabinet's buttons, the timer's box, the brick frames,
the apostrophe, RUB and END) and the sprites All-Stars has no art for (the high scores'
heads, the name entry's cursor) are the arcade's pixels and colours, lit from the top
left, shaded at the bottom right and outlined in a dark shade of their colour, as
All-Stars draws its sprites, put on layer 1 in whole cells: cells that come out the
same share a tile (the brick frames are a few hundred cells of a few tiles), and the
tiles share BG palettes 2-7 as their colours fit, filled anew for each picture (the
screen before's colours left in them drew the 2-player button half orange after the
game's end, and the cursor broken). Mario there is All-Stars' frame for
the pose VS. draws. VS. writes its PUSH BUTTON screen over the title's level without
moving the scroll, so that screen is told by its OperMode (1, from task 3: on the frame
before, the title still shows, banner and all). While the screen is off the picture is
black, as the SNES shows its forced blank; the arcade shows its backdrop's colour, which
between the attract's parts is the sky for a frame.

**Music.** All-Stars' music is its own sound program on an emulated SNES sound unit,
`spc700.c`: LakeSnes's SPC700, timers, ports and S-DSP (github.com/angelo-wf/LakeSnes,
MIT licence, its notice in the file), stepped cycle by cycle, the DSP making a sample
every 32 cycles. From the same snapshot of All-Stars' music its output is LakeSnes's bit
for bit and matches blargg's snes_spc, the core of most SNES players, to a correlation of
0.999. No boot ROM: the streams All-Stars uploads before Super Mario Bros. starts (blocks
of length, address and bytes, each ending with a jump to $0500) are written into its RAM
in its order: the engine with the menu's songs and samples ($07:FC00 and $3B:8000), the
instruments the games use, their directory at $3C00 and samples at $4000-$BC1F
($0B:8000, uploaded at the title, $00:8AA3), then Super Mario Bros.' songs ($1F:8000,
$00:8B17); the result is the sound unit's RAM in All-Stars itself (run in a reference
emulator) byte for byte, but for the program's variables. Each real frame (not
the run-ahead ones) the port routine All-Stars runs in its NMI ($04:8163) is repeated:
the song asked for is held on port 2 until the program echoes it, an effect on port 0
for up to 3 frames, then the sound unit runs a frame's 17,039 cycles and its 32 kHz
output goes, mixed to mono and resampled, on top of the APU's samples. The songs are
the ones All-Stars asks for the same music: VS.'s AreaMusicQueue ($FB) and
EventMusicQueue ($FC), read at the end of each frame, before the next NMI's SoundEngine
takes them, give ground $02, water $03, underground $06, castle $08, coin heaven $01,
pipe $13, star $0D, death $09, game over $0A, the castle's end $0B, the princess $0E, the
goal $0C, silence the program's stop $F0, and time running out effect $FF (the program
plays the jingle and speeds the song up); Bowser's battle song ($04, in world 8 $05)
starts when he appears, as All-Stars' Bowser code asks for it ($03:D6AB). After a state
load (a frame counter step other than one) the song is set again from VS.'s music
buffers. While All-Stars' music plays, the APU's squares and noise are heard only while
VS.'s sound effects use them (Square1/2SoundBuffer $F1/$F2, NoiseSoundBuffer $F3) and
its triangle not at all: `apu_mask_hook` picks the channels for each stretch between
register writes, and that mix goes through its own copy of the output filters, so the
APU's state (and the frame hashes, run-ahead, save states) is the same with the music on
or off.

Nothing from either ROM is stored: All-Stars' graphics, code and sound program are read
from the player's file at run time and kept in memory, and the tiles made for the
screens are made again for each frame in the copy of the PPU it is drawn from.

## Known inaccuracies

Blargg tests still failing, and why they do not matter for this game:

| test | what it checks |
|---|---|
| cpu_interrupts_v2 2-5 | sub-instruction NMI/IRQ/BRK/DMA interactions |
| ppu_vbl_nmi 05, 07, 08, 10 | NMI timing to the exact PPU dot, odd-frame skip vs. late BG enable |
| apu_test 7-dmc_basics | one DMC buffer-refill detail |
| mmc3_test_2 4, 6 | one scanline-timing sub-test; "alternate" (old revision) MMC3 behaviour |
| cpu_exec_space | executing code from I/O registers |
| ppu_open_bus, ppu_read_buffer | open-bus decay and a late read-buffer sub-test |
| apu_reset 4017_timing, irq_flag_cleared, works_immediately | reset-time APU details |
| instr_misc | the unstable SHA/SHX/SHY/TAS opcodes |

Other simplifications: sprite evaluation is done in one step at dot 257
(overflow flag without the hardware's buggy scan), the PPU warm-up period is
not enforced, DMC DMA does not stall the CPU, the APU uses a box-filter
resampler plus the 90 Hz / 440 Hz high-pass and 14 kHz low-pass stages, and the
RGB PPU's odd-frame behaviour is taken to be the 2C02's.

## Frontend features

**Cabinet controls.** `vs_input()` turns the bound buttons into the board's
inputs once per emulated frame: a press of Insert coin closes coin slot 1 for
exactly four frames, Start sets the left controller's Select line (1P start), the
2P start binding the right one's, and A, B and the stick go to the left
controller, or the right one on Luigi's turn in a two-player game.

**DIP switches.** The Cabinet tab maps to `sys.vs_dip` (`dip_bits()`), which the
game reads at reset. With free play set, the launcher's attract mode still runs as
a coin-op (the title and the demo only play while the board has no credits), and
a game starts with the real switches; during a game a change waits for the next
reset.

**No sprite limit.** Sprite evaluation keeps the hardware's overflow flag
behaviour but, with the option on, copies every sprite on the line into a
64-entry secondary OAM; sprites 9-64 are loaded at the end of the sprite fetch
window without touching the PPU address bus.

**Rewind.** Every other frame the frontend takes a state and pushes it into
`rewind.c`, which stores the XOR against the previous state as runs of unchanged
bytes and runs of literals (varint lengths). Stepping back applies the newest
record to the current state; while the key is held the frontend pops a record,
loads it, runs one frame and shows it: two frames of history per shown frame.
Fast-forward within 1.5 s after a rewind applies the same records again and
returns to exactly the state the rewind started from.

**Start + Select.** The chord opens the menu without the game seeing half of it:
the frontend keeps the state from before each of the last ten real frames with
the pad that frame ran with, and when the second button goes down it loads the
state from before the first one did (dropping any rewind records made in
between). A press held for ten frames or more counts as deliberate and is not
undone. Buttons held when the game resumes from the menu are masked until released.

**Frame pacing.** Windows phase-locks to the monitor's vblank (DXGI
`WaitForVBlank`) and presents just before it; Android locks each frame to the
Choreographer's frame deadlines (API 33, looked up with `dlsym`) and asks for the
highest display mode that is a multiple of 60 Hz; Linux and the Steam Deck pace by
the timer without vsync (or with vsync when Low-latency sync is off). LATENCY.md
has the measurements.

**Ports.** The same frontend builds for Linux (`tools/linux`: SDL2 linked
statically, so the binary needs only glibc), for the Steam Deck (`DECK_EDITION`, or
a Linux build that reads Valve's DMI board name: fullscreen, the aspect shaped like
the screen, pad-first hints, LT + RT for the menu) and for Android (`android/`: the
game as `libmain.so` under SDL's Java glue; the system document picker hands the
chosen dump back as a drop event; the phone edition adds `touchpad.c`'s on-screen
controller, whose buttons land in the same pad byte as a controller's).

**Updates.** `updater.c` runs its check and installation on a worker thread and
the menu polls its state. A build knows its version and repository from the
Makefile (or `build_apk.py`); the check reads `releases/latest` with a small JSON
scanner that only looks at the members it needs. Packages are unpacked in memory
(zip on Windows, gzip + tar on Linux, with the deflate decoder of `romzip.c`), so
no tool is started and only the program file is written. The swap is two renames
under a lock that the main thread takes at exit, so quitting can never land between
them. On Android the verified APK goes to a `PackageInstaller` session whose status
comes back to `VSSMBActivity` and from there to `updater.c` over JNI.

**Shaders.** `draw_game_ex` composes the picture into a render target when a
shader or an internal resolution is active: game frame (nearest / linear / sharp
prescale) into the destination rectangle, a glow pass, a scanline texture and the
vignette; then the target is drawn to the window either 1:1, scaled, or as a
32x24 mesh with barrel distortion (`SDL_RenderGeometry`), and the aperture mask
(one texel per screen pixel, MOD blend) goes on top.

## Where to take it next

* Enemies in Integer views wider than 16:9 still appear inside the band, 48 pixels
  past the old edge, where the game creates them; spawning them earlier would change
  the game itself. Hammer Bros and Piranha Plants are erased 16 pixels into the left
  band, as the game erases them.
* Walking into the band: riding a platform into it, and hammers and platforms touching
  the player while he is wholly in it, would each need patches of their own: their
  positions, sprites and boxes wrap the way his do. Hammers erase themselves off the
  screen; cannons fire only while on it; anything 72 pixels past the edge is erased
  (OffscreenBoundsCheck), as the game erases it, which only views wider than that show;
  fireballs still go at the screen's right edge (the right band shows the level ahead,
  where the game has made nothing for them to hit).
* Keeping the top score between sessions (the board keeps it only while powered).
* A 2P start button on the phone edition's on-screen controller.
