VS. Super Mario Bros. - native PC port
======================================

This program contains no game data. To play you need your own dump of the
VS. Super Mario Bros. arcade board: an iNES .nes file (on its own or in a .zip),
or MAME's set suprmrio.zip (or suprmrioa.zip, the set whose timer runs faster).
The program checks the hash and refuses anything else.

Getting started
---------------
1. Run vssmb.exe (Windows) or vssmb (Linux).
2. On the ROM tab choose "Choose ROM file..." and pick your .nes or .zip file, or
   drop the file onto the window, or start the program with the file as an argument.
3. Go to Play > Start game. The chosen file is remembered in vssmb.ini.
4. The cabinet starts on its "PUSH BUTTON" screen: press Start for one player
   (or 2 for two players). With coins switched on (Cabinet tab), press Insert
   coin (Right Shift / the pad's Back button) first.

Remastered graphics (optional)
------------------------------
With your own Super Mario All-Stars ROM (SNES, USA version; the Wii and Switch
Online copies of it and Super Mario All-Stars + Super Mario World, USA, also work)
the game can look like All-Stars' Super Mario Bros.
On the ROM tab choose "Choose All-Stars ROM..." and pick the .sfc file or a .zip
with it (or drop it onto the window). Its hash is checked and the path remembered;
then Graphics > Graphics switches between Arcade and All-Stars. Levels, sprites,
the status bar and the intermission are drawn by All-Stars' own code; the screens
only the arcade has (the title's "VS.", PUSH BUTTON, continue, the cabinet's
buttons) are drawn in All-Stars' style. The widescreen views work with it.

The ROM also brings All-Stars' music: Audio > Music chooses Arcade or All-Stars
(All-Stars' own songs, played by its sound program, with the arcade's sound
effects), or As the graphics (the default: All-Stars' music with the All-Stars
graphics, the arcade's with the arcade's). Either works with either graphics.

Menu (Esc in game, or Start + Insert coin together)
----------------------------------------------------
Play          Start / resume, reset, save state, load state, the save state slot
              (1-9, with the date the slot was last saved), end game, quit
Graphics      Display mode (windowed / fullscreen / fullscreen exclusive),
              resolution (auto, the sizes your display offers, and always
              1920x1080 and 3840x2160), window size 1x-6x (while the resolution
              is Auto), scaling (integer: exact pixel multiples, may leave
              borders / fit: fills the screen, keeps the shape / stretch: fills,
              distorts; with a widescreen aspect in fullscreen, Integer fills the
              screen with more of the level on each side), aspect ratio (square
              pixels 8:7 / 4:3 CRT / 16:9 or 16:10 widescreen: the level goes on
              beside the picture during play, with the enemies the game keeps
              there, also after a death or a pipe, and Mario can walk left into
              it to the edge of the picture; other screens get black bars,
              nothing is stretched; walk_to_edge=0 in vssmb.ini keeps the
              game's own edge),
              graphics (Arcade / All-Stars: shown once the All-Stars ROM is
              chosen on the ROM tab),
              colors (Arcade: the cabinet's RGB monitor / NES: the colours the
              console version shows), filter (nearest / linear / sharp),
              shader (off / scanlines / CRT / CRT curved / glow) and its strength,
              overscan crop, sprite limit (8 per line as on the hardware, or
              unlimited: no flicker), low-latency sync (phase-locks to the
              display), run-ahead (0-3 frames; 1 cancels the game's own input lag;
              off while the All-Stars graphics are on, since drawing them for the
              frames ahead takes too long: Arcade brings your setting back).
              The line under the rows says what the settings produce and why there
              are borders or bars when there are.
Audio         Volume, music (As the graphics / Arcade / All-Stars: shown once the
              All-Stars ROM is chosen), latency preset (Low / Normal / Safe)
Controls      Rebind the buttons, the 2P start button and the rewind / fast-forward
              keys (select a row, press Enter, press the key or pad button)
Cabinet       The board's DIP switches:
                Coinage         free play (the default), or coins and credits
                Lives           3 or 2
                Extra life for  100, 150, 200 or 250 coins collected
                Timer           normal or fast
                Continue with   4 or 3 lives
              The board reads its switches when it starts: in the launcher a
              change applies at once, during a game at the next reset
              (Play > Reset game, or Ctrl+R).
ROM           Choose or change the dump; choose the All-Stars ROM (remastered graphics)
Updates       This program's version; check for a newer one and install it (below)

Default keys
------------
Stick: arrow keys    A (jump): X    B (run, fireball): Z
Insert coin: Right Shift    1P start: Enter    2P start: 2
Rewind: hold Backspace (pad: RB)    Fast-forward: hold Tab (pad: RT, the right trigger)
Gamepad: A = jump, X = run (the button left of A, as on the NES pad), Back = Insert
coin, Start = 1P start, D-pad or left stick.
In a two-player game the controller (and the keyboard) plays whoever's turn it
is, so you can pass one pad between turns.
Start + Insert coin pressed together opens the menu (also Guide or LB, or Esc).
The game never sees that Start or that coin: the program steps back to the moment
before the first of the two went down. Buttons still held when you leave the menu
reach the game only after you let go of them.

Other keys: P pause, F5 save state, F8 load state, F6 / F7 previous / next save
state slot, F11 or Alt+Enter fullscreen, F12 screenshot (screenshots folder),
Ctrl+R reset.

Save states and rewind
----------------------
There are nine save state slots. F5 saves to the current slot, F8 loads it,
F6 / F7 change the slot; the Play tab shows the same. Hold the rewind key to go
back through the last several minutes of play; release it to continue from there.
Went back too far? Hold fast-forward within 1.5 s of letting go of rewind: it goes
forward again through the stretch you rewound and stops where the rewind began.
Anywhere else fast-forward speeds the game up (8x). Rewind can be switched off in
vssmb.ini (rewind_enabled=0).

Shaders
-------
Scanlines (the beam lines of a CRT), CRT (scanlines, an aperture-grille mask and a
soft glow), CRT curved (the same on a curved tube with a vignette and rounded
corners) and Glow alone. Strength scales all of them. They cost nothing noticeable
and input lag does not change.

Updates
-------
The Updates tab shows this program's version. Check for updates asks GitHub for the
newest release; the program goes online only when you press it. When there is a newer
version, Update now downloads its package for your system, checks it against the
SHA-256 checksums published with the release and puts the new program in place of
this one, with its README.txt (this file) in place of the old one. Your settings, saves
and ROM stay as they are, and the previous version is kept beside it as vssmb.exe.old
(vssmb.old on Linux, where the drive allows it): to go back, delete vssmb.exe and
rename the .old file. Restart now starts the new version at once (a game in progress
ends); otherwise it starts the next time you open the game. Nothing is installed until
you press Update now. The program's folder has to be writable for this; if it is not
(Program Files, for example), download the new version from the release page.

Files
-----
saves/<rom name>.state1-9  save state slots (F5 / F8, F6 / F7 to pick one)
screenshots/               F12 screenshots
vssmb.ini                  settings and the remembered ROM path
vssmb.exe.old              the previous version, after an update (vssmb.old on Linux)

These live next to the program when that folder is writable. If it is not (Program
Files, a read-only location, or the program was started from inside the zip),
they go to %APPDATA%\VSSMBPort\VSSuperMarioBros\ instead (on Linux
~/.local/share/VSSMBPort/VSSuperMarioBros/).

The ROM tab shows the SHA-1, MD5 and CRC32 of your dump and whether it matches a
known VS. Super Mario Bros. dump: the usual one (MAME's suprmrio set gives the same
image) or the one with the faster timer (MAME's suprmrioa, also known as the "[a1]"
dump). Its timer ticks every 17 frames where the usual one's ticks every 20, and every
13 where the usual one's ticks every 17 with the Timer switch on fast; the rest of
the game is the same.

Legal
-----
VS. Super Mario Bros. and Super Mario All-Stars are trademarks and copyrights of
Nintendo. This port is an unofficial fan project; it only runs with a dump you
provide. Do not share your ROM files.

The SNES sound unit that plays All-Stars' music is LakeSnes's emulation
(github.com/angelo-wf/LakeSnes), used under its licence:

    MIT License

    Copyright (c) 2021-2023 angelo_wf and contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
