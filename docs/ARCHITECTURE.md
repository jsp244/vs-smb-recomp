# Architecture

## Goals and constraints

1. The repository ships no game data and nothing derived from the ROM.
   Everything that comes out of the user's dump (the recompiled C, disassembly
   listings, the built executable) is generated locally and ignored by git.
2. The port must be *correct first*: anything the recompiler cannot translate
   must still run, and the translated code must be provably equivalent to a
   reference interpreter.
3. Ordinary tooling only: C11, GNU make, Python 3, SDL2.

The runtime, the frontend and the recompiler come from the StarTropics native
port (the same design); what is specific to this game is the Vs. System board,
its palette, the flat 32 KiB recompilation unit, the jump-table discovery, the
cabinet controls and the widescreen bands.

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
runtime/widescreen.c the level beside the picture, from the nametables
runtime/sys.c        power/reset, frame loop, save states
runtime/headless.c   command-line harness (tests, scripting, pokes, screenshots, traces, coverage)
runtime/recomp_rt.h  helpers used by the generated code
recomp/recomp.py     the static recompiler
frontend/sdl_main.c  SDL2 player: menu, cabinet controls, scaling and shader passes, run-ahead, save slots, rewind
frontend/ui.c        the tabbed menu (choices, numbers, key capture, text rows)
frontend/rewind.c    rewind history: XOR deltas, run-length coded, in a byte ring
```

Options that are not machine state (`ppu_no_sprite_limit`, the DIP switches in
`sys.vs_dip`) live outside the saved structs, so a save state or a rewind step
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
  to ROM need no dispatcher exit. (The 8 KiB "mmc3" layout of the StarTropics port
  is still there for other boards.)
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
death reload the level. On a mismatch, build the tracing variant
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
rendered and the oldest one; nothing before the start of the area), and a frame
whose playfield scroll does not agree with the screen position is not a level
(intermission, text screens), so its bands are black. Objects do not exist beyond
the screen until the game spawns them at its edge, so the bands are background
only. `nes-headless --widescreen FILE` writes the canvas and checks that its centre
is the PPU's frame and that the centre re-rendered from the nametables differs from
it only by the sprites (0.2% of the pixels in 1-1). The frontend shows the part a
16:9 or 16:10 picture needs at the 4:3 pixel shape (341 or 307 columns, 319 / 287
with the overscan crop), or with Integer scaling in fullscreen every column the
screen holds at the multiple.

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
`WaitForVBlank`) and presents just before it; LATENCY.md has the measurements.

**Shaders.** `draw_game_ex` composes the picture into a render target when a
shader or an internal resolution is active: game frame (nearest / linear / sharp
prescale) into the destination rectangle, a glow pass, a scanline texture and the
vignette; then the target is drawn to the window either 1:1, scaled, or as a
32x24 mesh with barrel distortion (`SDL_RenderGeometry`), and the aperture mask
(one texel per screen pixel, MOD blend) goes on top.

## Where to take it next

* Objects in the widescreen bands: SMB spawns enemies at the screen's edge, so
  they appear at the old edge; drawing them earlier would mean running the
  game's spawn and sprite code for a wider screen.
* Keeping the top score between sessions (the board keeps it only while powered).
* Linux, Steam Deck and Android packages, as for the StarTropics port.
