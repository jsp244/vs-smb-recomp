/*
 * png.c - minimal PNG writer (uncompressed deflate), used by the headless
 * harness and the screenshot key in the frontend.
 */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_update(uint32_t c, const uint8_t *buf, size_t len) {
    if (!crc_ready) crc_init();
    c = ~c;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void put32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    put32(f, (uint32_t)len);
    uint32_t c = crc32_update(0, (const uint8_t *)type, 4);
    c = crc32_update(c, data, len);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    put32(f, c);
}

int png_write_rgb(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 2, 0, 0, 0
    };
    chunk(f, "IHDR", ihdr, 13);

    size_t row = (size_t)w * 3 + 1;
    size_t raw_len = row * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        raw[y * row] = 0;
        memcpy(raw + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    /* zlib stream with stored blocks */
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen);
    size_t p = 0;
    z[p++] = 0x78; z[p++] = 0x01;
    size_t off = 0;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    while (off < raw_len) {
        size_t n = raw_len - off; if (n > 65535) n = 65535;
        z[p++] = (off + n == raw_len) ? 1 : 0;
        z[p++] = (uint8_t)n; z[p++] = (uint8_t)(n >> 8);
        z[p++] = (uint8_t)~n; z[p++] = (uint8_t)(~n >> 8);
        memcpy(z + p, raw + off, n); p += n; off += n;
    }
    uint32_t adler = (b << 16) | a;
    z[p++] = (uint8_t)(adler >> 24); z[p++] = (uint8_t)(adler >> 16); z[p++] = (uint8_t)(adler >> 8); z[p++] = (uint8_t)adler;
    chunk(f, "IDAT", z, p);
    chunk(f, "IEND", NULL, 0);
    free(z); free(raw);
    fclose(f);
    return 0;
}
