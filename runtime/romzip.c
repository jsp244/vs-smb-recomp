/*
 * romzip.c - the game's ROMs from zip archives.
 *
 * VS. Super Mario Bros. from MAME's sets, "suprmrio" and "suprmrioa": zips of the
 * six 8 KiB chips, put together into the same PRG + CHR image as an iNES dump
 * (with a NES 2.0 header for the Vs. System, mapper 99, RP2C04-0004). The chips
 * are found by their CRC32, so renamed files and merged sets work too, and a
 * split set's clone zip with its parent's.
 *
 * Or the one dump a zip holds: a .nes file, a Super Mario All-Stars ROM. The
 * caller judges the entries (romzip_pick).
 *
 * Only what such archives need: the zip directory, stored and deflated
 * entries (a small RFC 1951 decoder), CRC checks.
 */
#include "romzip.h"
#include <stdlib.h>
#include <string.h>

/* the chips in image order: PRG $8000, $A000, $C000, $E000, then CHR $0000, $1000. suprmrioa has its own
   $A000 chip (two bytes differ: how many frames each tick of the game timer lasts) and shares the rest. */
static const uint32_t chip_crc[2][6] = {
    { 0xbe4d5436u, 0x5e3fb550u, 0xb1b87893u, 0x1abf053cu, 0x42418d40u, 0x15506b86u },   /* suprmrio */
    { 0xbe4d5436u, 0x0011fc5au, 0xb1b87893u, 0x1abf053cu, 0x42418d40u, 0x15506b86u },   /* suprmrioa */
};
#define CHIP 8192

static uint32_t crc32_of(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

/* ---- inflate (RFC 1951): canonical Huffman codes decoded a bit at a time ---- */
typedef struct {
    const uint8_t *in; size_t in_len, in_pos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t out_len, out_pos;
    int bad;
} inflate_t;

typedef struct { uint16_t count[16]; uint16_t symbol[320]; } huff_t;

static int bits(inflate_t *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) { s->bad = 1; return 0; }
        v |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1));
}

/* lengths[0..n) -> counts per length and the symbols sorted by code; 0 if the code is over-subscribed */
static int huff_build(huff_t *h, const uint8_t *lengths, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = left * 2 - h->count[len];
        if (left < 0) return 0;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 1;
}

static int huff_decode(inflate_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->bad = 1;
    return 0;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(inflate_t *s, const huff_t *lit, const huff_t *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (s->bad) return 0;
        if (sym < 256) {
            if (s->out_pos >= s->out_len) return 0;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 1;
        } else {
            sym -= 257;
            if (sym >= 29) return 0;
            size_t len = len_base[sym] + (size_t)bits(s, len_extra[sym]);
            int ds = huff_decode(s, dist);
            if (s->bad || ds >= 30) return 0;
            size_t d = dist_base[ds] + (size_t)bits(s, dist_extra[ds]);
            if (s->bad || d > s->out_pos || s->out_pos + len > s->out_len) return 0;
            for (size_t i = 0; i < len; i++, s->out_pos++) s->out[s->out_pos] = s->out[s->out_pos - d];
        }
    }
}

static int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
    inflate_t s = { in, in_len, 0, 0, 0, out, out_len, 0, 0 };
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.bad) return 0;
        if (type == 0) {                       /* stored */
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.in_pos + 4 > s.in_len) return 0;
            size_t len = rd16(s.in + s.in_pos), nlen = rd16(s.in + s.in_pos + 2);
            s.in_pos += 4;
            if (len != (~nlen & 0xFFFF) || s.in_pos + len > s.in_len || s.out_pos + len > s.out_len) return 0;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len; s.out_pos += len;
        } else if (type == 1 || type == 2) {
            uint8_t lengths[320];
            huff_t lit, dist;
            int nlit, ndist;
            if (type == 1) {                   /* fixed codes */
                nlit = 288; ndist = 30;
                for (int i = 0; i < 288; i++) lengths[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                for (int i = 0; i < 30; i++) lengths[288 + i] = 5;
            } else {                           /* dynamic codes */
                nlit = bits(&s, 5) + 257; ndist = bits(&s, 5) + 1;
                int ncode = bits(&s, 4) + 4;
                if (s.bad || nlit > 286 || ndist > 30) return 0;
                uint8_t cl[19] = { 0 };
                for (int i = 0; i < ncode; i++) cl[order[i]] = (uint8_t)bits(&s, 3);
                huff_t clh;
                if (!huff_build(&clh, cl, 19)) return 0;
                for (int i = 0; i < nlit + ndist;) {
                    int sym = huff_decode(&s, &clh);
                    if (s.bad) return 0;
                    if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
                    int rep, val = 0;
                    if (sym == 16) { if (i == 0) return 0; val = lengths[i - 1]; rep = 3 + bits(&s, 2); }
                    else if (sym == 17) rep = 3 + bits(&s, 3);
                    else rep = 11 + bits(&s, 7);
                    if (i + rep > nlit + ndist) return 0;
                    while (rep--) lengths[i++] = (uint8_t)val;
                }
                if (lengths[256] == 0) return 0;
            }
            /* incomplete codes are allowed (a single distance code, for example), over-subscribed ones not */
            if (!huff_build(&lit, lengths, nlit) || !huff_build(&dist, lengths + nlit, ndist)) return 0;
            if (!inflate_codes(&s, &lit, &dist)) return 0;
        } else {
            return 0;
        }
    } while (!last);
    return s.out_pos == s.out_len;
}

int romzip_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) { return inflate_raw(in, in_len, out, out_len); }
uint32_t romzip_crc32(const uint8_t *data, size_t len) { return crc32_of(data, len); }

/* ---- zip directory ---- */
int romzip_is_zip(const uint8_t *data, size_t len) {
    return len >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
}

typedef struct {
    uint32_t flags, method, crc;
    size_t csize, usize, local, name_len;
    const uint8_t *name;
} zentry_t;

/* the end of the central directory: the last "PK\5\6" within the final 64 KiB + 22 bytes */
static int find_eocd(const uint8_t *zip, size_t len, size_t *eocd) {
    if (len < 22) return 0;
    for (size_t i = len - 22; ; i--) {
        if (rd32(zip + i) == 0x06054b50u) { *eocd = i; return 1; }
        if (i == 0 || len - i > 65535 + 22) return 0;
    }
}

/* the central directory's entry i; 0 when there is none */
static int zip_entry(const uint8_t *zip, size_t len, int want, zentry_t *e) {
    size_t eocd;
    if (want < 0 || !find_eocd(zip, len, &eocd)) return 0;
    size_t entries = rd16(zip + eocd + 10), p = rd32(zip + eocd + 16);
    for (size_t i = 0; i < entries; i++) {
        if (p > len || len - p < 46 || rd32(zip + p) != 0x02014b50u) return 0;
        size_t nlen = rd16(zip + p + 28), xlen = rd16(zip + p + 30), clen = rd16(zip + p + 32);
        if (len - p - 46 < nlen) return 0;
        if (i == (size_t)want) {
            e->flags = rd16(zip + p + 8);
            e->method = rd16(zip + p + 10);
            e->crc = rd32(zip + p + 16);
            e->csize = rd32(zip + p + 20);
            e->usize = rd32(zip + p + 24);
            e->local = rd32(zip + p + 42);
            e->name = zip + p + 46;
            e->name_len = nlen;
            return 1;
        }
        p += 46 + nlen + xlen + clen;
    }
    return 0;
}

/* entry e's data into out (e->usize bytes), checked against its CRC: stored or deflated, not encrypted */
static int zip_read(const uint8_t *zip, size_t len, const zentry_t *e, uint8_t *out) {
    if (e->flags & 1) return 0;
    if (e->local > len || len - e->local < 30 || rd32(zip + e->local) != 0x04034b50u) return 0;
    size_t data = e->local + 30 + rd16(zip + e->local + 26) + rd16(zip + e->local + 28);
    if (data > len || len - data < e->csize) return 0;
    int ok = 0;
    if (e->method == 0 && e->csize == e->usize) { memcpy(out, zip + data, e->usize); ok = 1; }
    else if (e->method == 8) ok = inflate_raw(zip + data, e->csize, out, e->usize);
    return ok && crc32_of(out, e->usize) == e->crc;
}

int romzip_entry(const uint8_t *zip, size_t len, int i, char *name, size_t name_size, size_t *size) {
    zentry_t e;
    if (!zip_entry(zip, len, i, &e)) return 0;
    if (name && name_size) {
        size_t n = e.name_len < name_size - 1 ? e.name_len : name_size - 1;
        memcpy(name, e.name, n);
        name[n] = 0;
    }
    if (size) *size = e.usize;
    return 1;
}

int romzip_pick(const uint8_t *zip, size_t len, size_t min, size_t max,
                int (*judge)(const char *name, const uint8_t *data, size_t size, void *ctx), void *ctx,
                uint8_t **out, size_t *out_len) {
    size_t eocd;
    if (!find_eocd(zip, len, &eocd)) return ROMZIP_NOT_ZIP;
    uint8_t *best = NULL;
    size_t best_len = 0;
    int unreadable = 0;
    zentry_t e;
    for (int i = 0; zip_entry(zip, len, i, &e); i++) {
        if (e.usize < min || e.usize > max || e.usize == 0) continue;
        char name[260];
        size_t n = e.name_len < sizeof name - 1 ? e.name_len : sizeof name - 1;
        memcpy(name, e.name, n);
        name[n] = 0;
        uint8_t *data = malloc(e.usize);
        if (!data) continue;
        if (!zip_read(zip, len, &e, data)) { free(data); unreadable = 1; continue; }
        int verdict = judge(name, data, e.usize, ctx);
        if (verdict == 2) { free(best); *out = data; *out_len = e.usize; return ROMZIP_OK; }
        if (verdict == 1 && !best) { best = data; best_len = e.usize; continue; }
        free(data);
    }
    if (best) { *out = best; *out_len = best_len; return ROMZIP_OK; }
    return unreadable ? ROMZIP_UNREADABLE : ROMZIP_MISSING;
}

int romzip_vs_set(const uint8_t *const zips[], const size_t lens[], int nzips, int set,
                  uint8_t **image, size_t *image_len, int *set_out) {
    size_t eocd;
    if (nzips < 1 || !find_eocd(zips[0], lens[0], &eocd)) return ROMZIP_NOT_ZIP;
    for (int k = 0; k < 2; k++) {
        int s = k == 0 ? set : 1 - set;
        if (s != ROMZIP_SET_SUPRMRIO && s != ROMZIP_SET_SUPRMRIOA) continue;
        uint8_t *img = calloc(1, 16 + 6 * CHIP);
        if (!img) return ROMZIP_MISSING;
        int have = 0, first = 0;
        for (int z = 0; z < nzips && have != 0x3F; z++) {
            zentry_t e;
            for (int i = 0; have != 0x3F && zip_entry(zips[z], lens[z], i, &e); i++) {
                if (e.usize != CHIP) continue;
                for (int c = 0; c < 6; c++)
                    if (!(have & (1 << c)) && chip_crc[s][c] == e.crc && zip_read(zips[z], lens[z], &e, img + 16 + (size_t)c * CHIP))
                        have |= 1 << c;
            }
            if (z == 0) first = have;
        }
        /* the first zip is the set chosen: it holds some of the chips at least (a parent's zip alone is not it) */
        if (have != 0x3F || !first) { free(img); continue; }
        /* NES 2.0 header: 2 x 16 KiB PRG, 2 x 8 KiB CHR, mapper 99, vertical, Vs. System with RP2C04-0004 */
        static const uint8_t hdr[16] = { 'N', 'E', 'S', 0x1A, 2, 2, 0x31, 0x69, 0, 0, 0, 0, 0, 0x05, 0, 0 };
        memcpy(img, hdr, 16);
        *image = img;
        *image_len = 16 + 6 * CHIP;
        if (set_out) *set_out = s;
        return ROMZIP_OK;
    }
    return ROMZIP_MISSING;
}
