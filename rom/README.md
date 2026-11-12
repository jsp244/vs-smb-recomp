# Put your own dump here

This folder is where `make recomp` and the game executable look for your
VS. Super Mario Bros. dump: an iNES `.nes` file (on its own or in a `.zip`), or
MAME's set `suprmrio.zip`. `make recomp` translates the first `.nes` or `.zip`
it finds (the two known dumps give the same native code). The file is yours: dump
it from a board you own.
Nothing in this folder is tracked by git (see `.gitignore`), and the project does
not contain, download, or write any game data.

Known dumps (ROM image without the 16-byte header: 32 KiB PRG, then 16 KiB CHR):

| Dump | SHA-1 of PRG+CHR data | CRC32 |
|---|---|---|
| VS. Super Mario Bros. (MAME `suprmrio`, chips MDS-SM4-4) | `b21aa940728ed80c72ee23c251c96e42cc84b2d6` | `98e3c75a` |
| VS. Super Mario Bros., faster timer (MAME `suprmrioa`, GoodNES "[a1]") | `91fa719b4b05adbac0b9d507d2051ed361d1ded4` | `7cff0f84` |

The second differs from the first in two bytes, the timer's tick (17 and 13
frames instead of 20 and 17). The native code is built from the first and reads
those two from whichever dump is loaded, so both run natively.

MAME's zip sets give the same images: the six chips are found by their CRC32
(PRG `be4d5436` `5e3fb550` `b1b87893` `1abf053c`, CHR `42418d40` `15506b86`;
`suprmrioa` has `0011fc5a` in place of `5e3fb550`) and put together in that
order. Merged sets work (`suprmrio.zip` gives the first set; the same zip named
`suprmrioa.zip` gives the second), and so does a split set's `suprmrioa.zip` with
`suprmrio.zip` beside it. Old iNES dumps of this game are often labelled mapper 3
with vertical mirroring and no Vs. System flag; the loader recognises the dump by
its hash and sets up the Vs. board (mapper 99, RP2C04-0004 PPU) regardless.
