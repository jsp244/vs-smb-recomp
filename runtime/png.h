#ifndef PNG_H
#define PNG_H
#include <stdint.h>
int png_write_rgb(const char *path, int w, int h, const uint8_t *rgb);
#endif
