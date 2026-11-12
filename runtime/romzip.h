/* romzip.h - the game's ROMs from zip archives: MAME's VS. Super Mario Bros. sets ("suprmrio.zip",
   "suprmrioa.zip") as an iNES image, or the dump a zip holds (a .nes file, an All-Stars ROM) */
#ifndef ROMZIP_H
#define ROMZIP_H
#include <stddef.h>
#include <stdint.h>

enum { ROMZIP_OK = 0, ROMZIP_NOT_ZIP = -1, ROMZIP_MISSING = -2, ROMZIP_UNREADABLE = -3 };

int romzip_is_zip(const uint8_t *data, size_t len);   /* starts like a zip archive */

/* MAME's two VS. Super Mario Bros. sets: 0 = "suprmrio", 1 = "suprmrioa" (its timer runs faster). */
enum { ROMZIP_SET_SUPRMRIO = 0, ROMZIP_SET_SUPRMRIOA = 1 };
/* finds the six chips of a set by CRC32 in the zips given (in that order: a split set's clone zip,
   then its parent), the set asked for first, else the other, and returns a malloc'd NES 2.0 image
   (header + PRG + CHR) and the set it is in *set_out; ROMZIP_MISSING when neither set is whole */
int romzip_vs_set(const uint8_t *const zips[], const size_t lens[], int nzips, int set,
                  uint8_t **image, size_t *image_len, int *set_out);

/* The files of a zip, in its directory's order. Entry i's name (cut to name_size) and size; 0 when
   there is no entry i. */
int romzip_entry(const uint8_t *zip, size_t len, int i, char *name, size_t name_size, size_t *size);
/* Of the entries sized [min, max]: each unpacked and handed to judge, which answers 2 (take it),
   1 (take it if no entry is better) or 0 (not it). The chosen one is returned malloc'd in *out;
   ROMZIP_MISSING when none fits, ROMZIP_UNREADABLE when the fitting ones cannot be unpacked (an
   unknown compression method, encryption, damage). */
int romzip_pick(const uint8_t *zip, size_t len, size_t min, size_t max,
                int (*judge)(const char *name, const uint8_t *data, size_t size, void *ctx), void *ctx,
                uint8_t **out, size_t *out_len);

/* the decoder and checksum on their own (the updater unpacks the game's packages with them):
   raw deflate data into out, which must be exactly the size of the result (1 when it is) */
int romzip_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
uint32_t romzip_crc32(const uint8_t *data, size_t len);
#endif
