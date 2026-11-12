/*
 * cart.c - iNES / NES 2.0 loader (also MAME's zip set, see romzip.c) and
 * battery RAM persistence.
 *
 * The ROM image is the user's own dump. Nothing from it is ever written
 * back into the repository; it is only kept in memory for the session.
 */
#include "nes.h"
#include "romzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

cart_t cart;

/* fopen that accepts UTF-8 paths on Windows (the C runtime's fopen is ANSI there) */
FILE *nes_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[4096], wmode[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 4096)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 16)) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

/* ---- SHA-1 (small, self-contained) ---- */
static uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void rom_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t total = len + 1;
    size_t padded = ((total + 8 + 63) / 64) * 64;
    uint8_t *buf = calloc(padded, 1);
    memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)buf[off + i * 4] << 24 | (uint32_t)buf[off + i * 4 + 1] << 16 |
                   (uint32_t)buf[off + i * 4 + 2] << 8 | buf[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(buf);
    uint32_t h[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i];
    }
}

/* ---- MD5 (RFC 1321) ---- */
void rom_md5(const uint8_t *data, size_t len, uint8_t out[16]) {
    static const uint8_t R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                   4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    static uint32_t K[64];
    static int k_ready;
    if (!k_ready) { for (int i = 0; i < 64; i++) K[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0); k_ready = 1; }
    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    uint8_t *buf = calloc(padded, 1);
    memcpy(buf, data, len);
    buf[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 8 + i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)buf[off + i * 4] | (uint32_t)buf[off + i * 4 + 1] << 8 |
                   (uint32_t)buf[off + i * 4 + 2] << 16 | (uint32_t)buf[off + i * 4 + 3] << 24;
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else             { f = c ^ (b | ~d); g = (7 * i) & 15; }
            uint32_t t = d;
            d = c; c = b;
            uint32_t x = a + f + K[i] + M[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    free(buf);
    uint32_t h[4] = { h0, h1, h2, h3 };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (8 * j));
}

uint32_t rom_crc32(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* Known VS. Super Mario Bros. dumps (ROM image without header: 32 KiB PRG, then 16 KiB CHR). The first
   is MAME's "suprmrio" set (chips MDS-SM4-4 1D 1C 1B 1A, 2B 2A, in that order); the second its
   "suprmrioa" (the GoodNES "[a1]" dump), whose 1C differs in two operands: the frames per tick of the
   game timer, 17 and 13 where the first has 20 and 17 (normal, fast). The program is the same apart
   from those, so the hooks, the patches and the native code (which reads them from the ROM, see
   recomp_fits_cart) fit both. */
static const struct { const char *sha1; const char *name; } known_roms[] = {
    { "b21aa940728ed80c72ee23c251c96e42cc84b2d6", "VS. Super Mario Bros." },
    { "91fa719b4b05adbac0b9d507d2051ed361d1ded4", "VS. Super Mario Bros. (faster timer)" },
    { NULL, NULL }
};

static const char *known_name_of(const char *sha1_hex) {
    for (int i = 0; known_roms[i].sha1; i++)
        if (strcmp(known_roms[i].sha1, sha1_hex) == 0) return known_roms[i].name;
    return NULL;
}

const char *cart_known_name(void) { return known_name_of(cart.sha1_hex); }

/* The native code fits the loaded dump: it is the dump the code was generated from, or one that differs
   from it only in the operands the code reads from the ROM (symbols.txt "rom"), which recomp.py hashes
   as zeros. */
int recomp_fits_cart(void) {
    if (!recomp_rom_sha1 || !cart.prg) return 0;
    if (strcmp(recomp_rom_sha1, cart.sha1_hex) == 0) return 1;
    if (!recomp_rom_sha1_masked || !recomp_rom_operands[0] || cart.prg_size != 0x8000) return 0;
    size_t n = cart.prg_size + cart.chr_size;
    uint8_t *img = malloc(n);
    if (!img) return 0;
    memcpy(img, cart.prg, cart.prg_size);
    if (cart.chr_size) memcpy(img + cart.prg_size, cart.chr, cart.chr_size);
    for (int i = 0; recomp_rom_operands[i]; i++) img[recomp_rom_operands[i] - 0x8000] = 0;
    uint8_t sha[20];
    char hex[41];
    rom_sha1(img, n, sha);
    free(img);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", sha[i]);
    return strcmp(hex, recomp_rom_sha1_masked) == 0;
}

/* an iNES image's PRG + CHR: where they start and how long they are together; 0 when it is no iNES
   image or is shorter than its header says */
static int ines_body(const uint8_t *h, size_t len, size_t *off, size_t *size) {
    if (len < 16 || memcmp(h, "NES\x1a", 4) != 0) return 0;
    size_t prg_units = h[4], chr_units = h[5];
    if ((h[7] & 0x0C) == 0x08) {
        prg_units |= (size_t)(h[9] & 0x0F) << 8;
        chr_units |= (size_t)(h[9] & 0xF0) << 4;
    }
    *off = 16 + ((h[6] & 4) ? 512 : 0);
    *size = prg_units * 16384 + chr_units * 8192;
    return *size && *off + *size <= len;
}

/* romzip_pick's judge for a dump in a zip: a known one at once, else the first iNES file */
static int judge_nes(const char *name, const uint8_t *d, size_t n, void *ctx) {
    (void)name; (void)ctx;
    size_t off, size;
    if (!ines_body(d, n, &off, &size)) return 0;
    uint8_t sha[20];
    char hex[41];
    rom_sha1(d + off, size, sha);
    for (int i = 0; i < 20; i++) sprintf(hex + i * 2, "%02x", sha[i]);
    return known_name_of(hex) ? 2 : 1;
}

/* the file name without its folder */
static const char *path_base(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* s begins with prefix, ignoring ASCII case */
static int starts_nocase(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++) {
        int a = (unsigned char)*s, b = (unsigned char)*prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static uint8_t *read_whole(const char *path, size_t max, size_t *len) {
    FILE *f = nes_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (n > 0 && (size_t)n <= max) ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *len = (size_t)n;
    return d;
}

/* A zip: MAME's set (the one its name asks for: suprmrioa.zip the faster timer's), else the .nes dump in
   it, else a split set's clone zip with its parent's chips from suprmrio.zip in the same folder. */
static int unzip_dump(const char *path, const uint8_t *zip, size_t len, uint8_t **img, size_t *n) {
    const char *base = path_base(path);
    int set = starts_nocase(base, "suprmrioa") ? ROMZIP_SET_SUPRMRIOA : ROMZIP_SET_SUPRMRIO;
    const uint8_t *zips[2] = { zip, NULL };
    size_t lens[2] = { len, 0 };
    if (romzip_vs_set(zips, lens, 1, set, img, n, NULL) == ROMZIP_OK) return ROMZIP_OK;
    int rc = romzip_pick(zip, len, 16 + 16384, 0x100000 + 16 + 512, judge_nes, NULL, img, n);
    if (rc == ROMZIP_OK || (starts_nocase(base, "suprmrio.zip") && !base[12])) return rc;
    char parent[4096];
    snprintf(parent, sizeof parent, "%.*ssuprmrio.zip", (int)(base - path), path);
    size_t plen;
    uint8_t *p = read_whole(parent, 64u << 20, &plen);
    if (!p) return rc;
    zips[1] = p;
    lens[1] = plen;
    int r2 = romzip_vs_set(zips, lens, 2, set, img, n, NULL);
    free(p);
    return r2 == ROMZIP_OK ? ROMZIP_OK : rc;
}

int cart_load(const char *path) {
    FILE *f = nes_fopen(path, "rb");
    if (!f) { fprintf(stderr, "cart: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 22) { fclose(f); fprintf(stderr, "cart: file too small\n"); return -1; }
    if (len > (64L << 20)) { fclose(f); fprintf(stderr, "cart: file too large\n"); return -1; }
    uint8_t *data = malloc((size_t)len);
    if (fread(data, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(data); return -1; }
    fclose(f);

    if (romzip_is_zip(data, (size_t)len)) {
        /* MAME's set (the chips put together into the image an iNES dump holds), or a zipped .nes dump */
        uint8_t *img;
        size_t n;
        int rc = unzip_dump(path, data, (size_t)len, &img, &n);
        free(data);
        if (rc != ROMZIP_OK) {
            fprintf(stderr, "cart: %s holds no .nes dump and no MAME VS. Super Mario Bros. set (suprmrio, suprmrioa)%s\n",
                    path, rc == ROMZIP_UNREADABLE ? " this program can unpack (stored or deflated files only)" : "");
            return -1;
        }
        data = img;
        len = (long)n;
    }
    if (len < 16 + 16384) { free(data); fprintf(stderr, "cart: file too small\n"); return -1; }
    if (memcmp(data, "NES\x1a", 4) != 0) { free(data); fprintf(stderr, "cart: not an iNES file\n"); return -1; }
    cart_free();
    memset(&cart, 0, sizeof cart);
    snprintf(cart.path, sizeof cart.path, "%s", path);

    uint8_t *h = data;
    uint32_t prg_units = h[4], chr_units = h[5];
    cart.nes2 = (h[7] & 0x0C) == 0x08;
    cart.mapper = (h[6] >> 4) | (h[7] & 0xF0);
    cart.mirroring = h[6] & 1;
    cart.battery = (h[6] & 2) != 0;
    cart.four_screen = (h[6] & 8) != 0;
    bool trainer = (h[6] & 4) != 0;
    if (cart.nes2) {
        cart.mapper |= (h[8] & 0x0F) << 8;
        cart.submapper = h[8] >> 4;
        prg_units |= (uint32_t)(h[9] & 0x0F) << 8;
        chr_units |= (uint32_t)(h[9] & 0xF0) << 4;
    } else {
        /* Old-style dumps often carry junk in bytes 8..15 ("DiskDude!"): ignore the high mapper nibble then. */
        bool junk = false;
        for (int i = 8; i < 16; i++) if (h[i]) junk = true;
        if (junk) cart.mapper = h[6] >> 4;
    }
    cart.prg_size = prg_units * 16384;
    cart.chr_size = chr_units * 8192;
    /* Vs. System: NES 2.0 console type 1 (byte 13 = PPU type), the old Vs. flag, or mapper 99 */
    cart.vs_ppu = -1;
    if (cart.nes2 && (h[7] & 3) == 1) { cart.vs = true; cart.vs_ppu = h[13] & 0x0F; }
    else if (!cart.nes2 && (h[7] & 1)) cart.vs = true;
    if (cart.mapper == 99) cart.vs = true;

    size_t off = 16 + (trainer ? 512 : 0);
    if (off + cart.prg_size + cart.chr_size > (size_t)len) {
        fprintf(stderr, "cart: truncated ROM (expected %u bytes of PRG+CHR)\n", cart.prg_size + cart.chr_size);
        free(data); return -1;
    }
    cart.prg = malloc(cart.prg_size);
    memcpy(cart.prg, data + off, cart.prg_size);
    if (cart.chr_size) {
        cart.chr = malloc(cart.chr_size);
        memcpy(cart.chr, data + off + cart.prg_size, cart.chr_size);
    }
    rom_sha1(data + off, cart.prg_size + cart.chr_size, cart.sha1);
    for (int i = 0; i < 20; i++) sprintf(cart.sha1_hex + i * 2, "%02x", cart.sha1[i]);
    rom_md5(data + off, cart.prg_size + cart.chr_size, cart.md5);
    for (int i = 0; i < 16; i++) sprintf(cart.md5_hex + i * 2, "%02x", cart.md5[i]);
    cart.crc32 = rom_crc32(data + off, cart.prg_size + cart.chr_size);
    free(data);

    /* MMC6: NES 2.0 submapper 1 of mapper 4 */
    cart.mmc6 = (cart.mapper == 4 && cart.submapper == 1);
    if (cart.mmc6) cart.battery = true;
    /* a known VS. Super Mario Bros. dump: the Vs. board with its RP2C04-0004 PPU, whatever the header
       says (common old dumps are labelled mapper 3 with vertical mirroring and no Vs. flag) */
    if (cart_known_name()) { cart.mapper = 99; cart.vs = true; cart.vs_ppu = 5; cart.battery = false; }
    if (cart.vs && cart.vs_ppu < 0) cart.vs_ppu = 0;   /* unknown: the RP2C03B, whose palette is the plain RGB one */

    memset(cart.wram, 0, sizeof cart.wram);
    memset(cart.chr_ram, 0, sizeof cart.chr_ram);
    return 0;
}

void cart_free(void) {
    free(cart.prg); cart.prg = NULL;
    free(cart.chr); cart.chr = NULL;
}

static size_t battery_size(void) { return cart.mmc6 ? 1024 : 8192; }
size_t cart_battery_size(void) { return battery_size(); }

int cart_load_battery(const char *path) {
    FILE *f = nes_fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(cart.wram, 1, battery_size(), f);
    fclose(f);
    return n == battery_size() ? 0 : -1;
}

int cart_save_battery(const char *path) {
    if (!cart.battery) return 0;
    FILE *f = nes_fopen(path, "wb");
    if (!f) return -1;
    fwrite(cart.wram, 1, battery_size(), f);
    fclose(f);
    return 0;
}
