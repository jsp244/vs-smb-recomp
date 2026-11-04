# Put your own dump here

This folder is where `make recomp` and the game executable look for your
VS. Super Mario Bros. dump: an iNES `.nes` file, or MAME's set `suprmrio.zip`.
The file is yours: dump it from a board you own. Nothing in this folder is
tracked by git (see `.gitignore`), and the project does not contain, download,
or write any game data.

Expected dump (ROM image without the 16-byte header: 32 KiB PRG, then 16 KiB CHR):

| Dump | SHA-1 of PRG+CHR data | CRC32 |
|---|---|---|
| VS. Super Mario Bros. (MAME `suprmrio`, chips MDS-SM4-4) | `b21aa940728ed80c72ee23c251c96e42cc84b2d6` | `98e3c75a` |

MAME's zip set gives the same image: the six chips are found by their CRC32
(PRG `be4d5436` `5e3fb550` `b1b87893` `1abf053c`, CHR `42418d40` `15506b86`) and
put together in that order. Old iNES dumps of this game are often labelled
mapper 3 with vertical mirroring and no Vs. System flag; the loader recognises
the dump by its hash and sets up the Vs. board (mapper 99, RP2C04-0004 PPU)
regardless.
