/* romzip.h - MAME's VS. Super Mario Bros. set ("suprmrio.zip") as an iNES image */
#ifndef ROMZIP_H
#define ROMZIP_H
#include <stddef.h>
#include <stdint.h>

enum { ROMZIP_OK = 0, ROMZIP_NOT_ZIP = -1, ROMZIP_MISSING = -2 };

int romzip_is_zip(const uint8_t *data, size_t len);   /* starts like a zip archive */
/* finds the six chips by CRC32 and returns a malloc'd NES 2.0 image (header + PRG + CHR);
   ROMZIP_MISSING when a chip is absent or fails its CRC */
int romzip_vs_set(const uint8_t *zip, size_t len, uint8_t **image, size_t *image_len);
#endif
