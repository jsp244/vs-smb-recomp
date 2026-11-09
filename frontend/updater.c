/*
 * updater.c - the Updates tab (updater.h).
 *
 * Check for updates: api.github.com/repos/<VSSMB_REPO>/releases/latest, the newest published
 * release (drafts and pre-releases never are) of the repository the build came from (the
 * Makefile and build_apk.py take it from the git remote); its tag is compared with
 * VSSMB_VERSION (git describe: the newest v* tag, plus the commits after it).
 *
 * Update now: downloads this system's package from that release (VSSMB_PACKAGE, the name
 * `make dist` gives it; on Android the edition's APK) and nothing else: its address must be
 * one of the release's own downloads. The download must have the size GitHub lists and the
 * SHA-256 the release publishes for it: the digest GitHub keeps for every uploaded file and the
 * release's SHA256SUMS file, both checked where the release has them; a package the release
 * gives no SHA-256 for is not installed. Then:
 *  - Windows: the program is taken out of the zip in memory and written beside the running one
 *    as <name>.new; the running program is renamed to <name>.old (Windows allows renaming a
 *    running executable) and the new one takes its place. Restart now starts it in a new process.
 *  - Linux and the Steam Deck: the same from the tar.gz; the running program keeps a second name,
 *    <name>.old (a hard link, where the drive allows one), and <name>.new is renamed over it,
 *    which swaps it in one step. Restart now exec()s the new version in the same process, so a
 *    Steam session carries on.
 *  - Android: the APK goes to the system's package installer (VSSMBActivity.installUpdate),
 *    which asks the player to confirm, closes the game and installs the update over it.
 * On the desktop the player's notes in the package (README.txt, which carries the licences of
 * the code in the program, and STEAMDECK.txt in the Deck edition) are written the same way,
 * <name>.new renamed over <name>, and before the program: a folder never runs a new version
 * with the old notes. Only those names are written. Settings, saves and the ROM are never
 * touched. Checks and installations run on a worker thread; the menu reads the state on the
 * main thread.
 */
#include "updater.h"
#include "http.h"
#include "romzip.h"

#include <SDL.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#ifdef __ANDROID__
#  include <jni.h>
#endif

#ifndef VSSMB_VERSION
#  define VSSMB_VERSION "dev"
#endif
#ifndef VSSMB_REPO
#  define VSSMB_REPO ""         /* owner/name on GitHub; empty when the build did not know it */
#endif
#ifndef VSSMB_PACKAGE
#  define VSSMB_PACKAGE ""      /* the release asset for this system (desktop builds) */
#endif

#ifdef _WIN32
#  define SEP "\\"
#else
#  define SEP "/"
#endif
/* the program inside every desktop package, in its folder (a test build can name another
   project's, to rehearse an installation against that project's releases) */
#ifndef BINARY
#  ifdef _WIN32
#    define BINARY "vssmb.exe"
#  else
#    define BINARY "vssmb"
#  endif
#endif
#ifndef PACKAGE_DIR
#  define PACKAGE_DIR "vssmb"
#endif
#ifndef __ANDROID__
/* the player's notes beside the program in the desktop packages (the Makefile's dist rule):
   an update writes these too, and no other names */
static const char *const doc_names[] = {
    "README.txt",
#  ifdef DECK_EDITION
    "STEAMDECK.txt",
#  endif
};
#  define N_DOCS (sizeof doc_names / sizeof doc_names[0])
#  ifdef DECK_EDITION
#    define DOCS_TEXT "README.txt and STEAMDECK.txt"
#  else
#    define DOCS_TEXT "README.txt"
#  endif
#endif
#define SUMS_NAME "SHA256SUMS"  /* the checksum file published with a release */
#define TIMEOUT_MS 20000
#define MAX_PACKAGE ((size_t)64 << 20)
#define MAX_UNPACKED ((uint32_t)256 << 20)

#if defined(__GNUC__)
#  define UPD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define UPD_PRINTF(f, a)
#endif

static SDL_mutex *g_lock;            /* guards the state below; made by updater_startup */
static update_info g_info;
static char g_url[512], g_sums_url[512], g_digest[80], g_tag[64];
static long long g_size;
static SDL_atomic_t g_got, g_total;  /* download progress; g_got -1 once the download is done */
static SDL_atomic_t g_cancel;        /* the program is shutting down */
static SDL_mutex *g_swap;            /* held while the new version is moved into place */
static char g_pretend[64];           /* --update-test */
static char g_exe[1024];             /* this program, found on the main thread before any worker runs */
static int g_touch;

static void lock(void) {
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (g_lock) SDL_LockMutex(g_lock);
}

static void unlock(void) {
    if (g_lock) SDL_UnlockMutex(g_lock);
}

static void UPD_PRINTF(2, 3) set_state(int state, const char *fmt, ...) {
    va_list ap;
    lock();
    g_info.state = state;
    va_start(ap, fmt);
    vsnprintf(g_info.text, sizeof g_info.text, fmt, ap);
    va_end(ap);
    unlock();
}

const char *updater_version(void) {
    const char *v = g_pretend[0] ? g_pretend : VSSMB_VERSION;
    return (v[0] == 'v' || v[0] == 'V') && isdigit((unsigned char)v[1]) ? v + 1 : v;
}

const char *updater_edition(void) {
#if defined(__ANDROID__)
    return g_touch ? "Android, phone" : "Android";
#elif defined(_WIN32)
    return "Windows";
#elif defined(DECK_EDITION)
    return "Steam Deck";
#else
    return "Linux";
#endif
}

void updater_set_touch(int touch) { g_touch = touch; }

static const char *package_name(void) {
#ifdef __ANDROID__
    return g_touch ? "vssmb-android-phone.apk" : "vssmb-android-handheld.apk";   /* android/build_apk.py */
#else
    return VSSMB_PACKAGE;
#endif
}

static const char *user_agent(void) {
    static char ua[96];
    if (!ua[0]) snprintf(ua, sizeof ua, "VSSMBPort/%s", updater_version());
    return ua;
}

static int same_text_nocase(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

/* ---- versions ---- */

/* "v1.2.3-4-gabcdef1" -> {1, 2, 3} and the 4 commits after the tag; 0 when it does not begin with a number */
static int parse_version(const char *s, long v[3], long *after) {
    char *end;
    v[0] = v[1] = v[2] = 0;
    *after = 0;
    if (*s == 'v' || *s == 'V') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    for (int i = 0; i < 3 && isdigit((unsigned char)*s); i++) {
        v[i] = strtol(s, &end, 10);
        s = end;
        if (*s != '.') break;
        s++;
    }
    if (*s == '-' && isdigit((unsigned char)s[1])) *after = strtol(s + 1, NULL, 10);
    return 1;
}

/* > 0 when version a is newer than b, < 0 when older; a build without a version is older than any */
static int compare_versions(const char *a, const char *b) {
    long va[3], vb[3], aa, ab;
    int pa = parse_version(a, va, &aa), pb = parse_version(b, vb, &ab);
    if (!pa || !pb) return pa - pb;
    for (int i = 0; i < 3; i++)
        if (va[i] != vb[i]) return va[i] > vb[i] ? 1 : -1;
    return (aa > ab) - (aa < ab);
}

/* ---- the bits of JSON a release needs ---- */

static const char *ws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *skip_string(const char *p, const char *e) {   /* p at the opening quote */
    for (p++; p < e; p++) {
        if (*p == '\\') p++;
        else if (*p == '"') return p + 1;
    }
    return e;
}

static const char *skip_value(const char *p, const char *e) {
    p = ws(p, e);
    if (p >= e) return e;
    if (*p == '"') return skip_string(p, e);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < e) {
            if (*p == '"') { p = skip_string(p, e); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return e;
    }
    while (p < e && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;   /* number, true, false, null */
    return p;
}

/* the value of key among the object's own members (not those of objects inside it), or NULL */
static const char *member(const char *p, const char *e, const char *key) {
    size_t klen = strlen(key);
    p = p ? ws(p, e) : e;
    if (p >= e || *p != '{') return NULL;
    for (p++;;) {
        p = ws(p, e);
        if (p >= e || *p != '"') return NULL;
        const char *k = p + 1, *after = skip_string(p, e);
        int match = (size_t)(after - 1 - k) == klen && !memcmp(k, key, klen);
        p = ws(after, e);
        if (p >= e || *p != ':') return NULL;
        p = ws(p + 1, e);
        if (match) return p;
        p = ws(skip_value(p, e), e);
        if (p >= e || *p != ',') return NULL;
        p++;
    }
}

static void string_value(const char *p, const char *e, char *out, size_t cap) {
    size_t n = 0;
    if (p && p < e && *p == '"') {
        for (p++; p < e && *p != '"'; p++) {
            char c = *p;
            if (c == '\\' && p + 1 < e) {
                c = *++p;
                if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') c = ' ';
                else if (c == 'u' && e - p > 4) {
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    unsigned long u = strtoul(hex, NULL, 16);
                    c = u < 128 ? (char)u : '?';
                    p += 4;
                }
            }
            if (n + 1 < cap) out[n++] = c;
        }
    }
    if (cap) out[n] = '\0';
}

/* ---- SHA-256 ---- */

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = s[7] + (ROR(s[4], 6) ^ ROR(s[4], 11) ^ ROR(s[4], 25)) + ((s[4] & s[5]) ^ (~s[4] & s[6])) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(s[0], 2) ^ ROR(s[0], 13) ^ ROR(s[0], 22)) + ((s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]));
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}

static void sha256_hex(const uint8_t *data, size_t len, char hex[65]) {
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    uint8_t tail[128] = { 0 };
    size_t full = len / 64 * 64, rest = len - full, tlen = rest < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (size_t i = 0; i < full; i += 64) sha256_block(h, data + i);
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tlen; i += 64) sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++) snprintf(hex + i * 8, 9, "%08x", (unsigned)h[i]);
}

/* the SHA-256 a SHA256SUMS file (sha256sum's format: "<hex>  <name>" per line) gives for name */
static int sums_lookup(const char *text, size_t len, const char *name, char hex[65]) {
    const char *p = text, *e = text + len;
    size_t nlen = strlen(name);
    while (p < e) {
        const char *eol = memchr(p, '\n', (size_t)(e - p)), *q = p, *f;
        size_t n = 0;
        if (!eol) eol = e;
        while (q < eol && n < 64 && isxdigit((unsigned char)*q)) { q++; n++; }
        if (n == 64 && q < eol && (*q == ' ' || *q == '\t')) {
            const char *fe = eol;
            for (f = q; f < eol && (*f == ' ' || *f == '\t'); f++) {}
            if (f < eol && *f == '*') f++;   /* binary-mode mark */
            if (fe > f && fe[-1] == '\r') fe--;
            if ((size_t)(fe - f) == nlen && !memcmp(f, name, nlen)) {
                for (int i = 0; i < 64; i++) hex[i] = (char)tolower((unsigned char)p[i]);
                hex[64] = '\0';
                return 1;
            }
        }
        p = eol + 1;
    }
    return 0;
}

/* ---- the packages: the program out of a zip (Windows) or a tar.gz (Linux), in memory ---- */

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

/* a malloc'd copy of the zip entry called name, checked against its CRC-32, or NULL */
static uint8_t *zip_entry(const uint8_t *zip, size_t len, const char *name, size_t *out_len) {
    size_t eocd = 0, nlen = strlen(name);
    int found = 0;
    for (size_t i = len >= 22 ? len - 22 : 0;; i--) {   /* end of the central directory: within the last 64 KiB + 22 bytes */
        if (i + 22 <= len && rd32(zip + i) == 0x06054b50u) { eocd = i; found = 1; break; }
        if (i == 0 || len - i > 65535 + 22) break;
    }
    if (!found) return NULL;
    size_t entries = rd16(zip + eocd + 10), p = rd32(zip + eocd + 16);
    for (size_t k = 0; k < entries; k++) {
        if (p + 46 > len || rd32(zip + p) != 0x02014b50u) return NULL;
        uint32_t flags = rd16(zip + p + 8), method = rd16(zip + p + 10), crc = rd32(zip + p + 16);
        size_t csize = rd32(zip + p + 20), usize = rd32(zip + p + 24), local = rd32(zip + p + 42);
        size_t fn = rd16(zip + p + 28), extra = rd16(zip + p + 30), comment = rd16(zip + p + 32);
        int match = p + 46 + fn <= len && fn == nlen && !memcmp(zip + p + 46, name, nlen);
        p += 46 + fn + extra + comment;
        if (!match) continue;
        if ((flags & 1) || usize > MAX_UNPACKED || local + 30 > len || rd32(zip + local) != 0x04034b50u) return NULL;
        size_t data = local + 30 + rd16(zip + local + 26) + rd16(zip + local + 28);
        if (data > len || csize > len - data) return NULL;
        uint8_t *out = (uint8_t *)malloc(usize ? usize : 1);
        int ok = 0;
        if (!out) return NULL;
        if (method == 0 && csize == usize) { memcpy(out, zip + data, usize); ok = 1; }
        else if (method == 8) ok = romzip_inflate(zip + data, csize, out, usize);
        if (!ok || romzip_crc32(out, usize) != crc) { free(out); return NULL; }
        *out_len = usize;
        return out;
    }
    return NULL;
}

/* the contents of a gzip file (one member, as Python's tarfile writes it), checked against its CRC-32 */
static uint8_t *gunzip(const uint8_t *gz, size_t len, size_t *out_len) {
    size_t p = 10;
    if (len < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) return NULL;
    uint8_t flags = gz[3];
    if (flags & 4) { if (p + 2 > len) return NULL; p += 2 + rd16(gz + p); }        /* extra field */
    for (int f = 8; f <= 16; f <<= 1)                                               /* file name, comment */
        if (flags & f) { while (p < len && gz[p]) p++; p++; }
    if (flags & 2) p += 2;                                                          /* header CRC */
    if (p + 8 > len) return NULL;
    uint32_t crc = rd32(gz + len - 8), size = rd32(gz + len - 4);
    if (size > MAX_UNPACKED) return NULL;
    uint8_t *out = (uint8_t *)malloc(size ? size : 1);
    if (!out) return NULL;
    if (!romzip_inflate(gz + p, len - 8 - p, out, size) || romzip_crc32(out, size) != crc) { free(out); return NULL; }
    *out_len = size;
    return out;
}

static int octal(const uint8_t *p, size_t n, size_t *value) {
    size_t v = 0, i = 0;
    while (i < n && p[i] == ' ') i++;
    if (i == n || p[i] < '0' || p[i] > '7') return 0;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > ((size_t)-1 >> 3)) return 0;
        v = v << 3 | (size_t)(p[i] - '0');
    }
    *value = v;
    return 1;
}

/* a malloc'd copy of the regular file called name in a tar archive (ustar, with the pax and GNU
   long-name records Python's tarfile adds), or NULL */
static uint8_t *tar_entry(const uint8_t *tar, size_t len, const char *name, size_t *out_len) {
    char longname[300] = "";
    for (size_t p = 0; p + 512 <= len;) {
        const uint8_t *h = tar + p;
        char entry[300];
        size_t size, sum = 0, want;
        int zero = 1;
        for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
        if (zero) return NULL;                                  /* the end of the archive */
        for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
        if (!octal(h + 148, 8, &want) || want != sum || !octal(h + 124, 12, &size)) return NULL;
        size_t data = p + 512;
        if (size > len - data) return NULL;
        size_t next = data + (size + 511) / 512 * 512;
        char type = (char)h[156];
        if (longname[0]) {
            snprintf(entry, sizeof entry, "%s", longname);
            longname[0] = '\0';
        } else if (!memcmp(h + 257, "ustar", 5) && h[345]) {
            snprintf(entry, sizeof entry, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        } else {
            snprintf(entry, sizeof entry, "%.100s", (const char *)h);
        }
        if (type == 'x') {                                      /* pax: a path= record names the next entry */
            for (size_t q = data; q < data + size;) {
                size_t rlen = 0, k = q;
                while (k < data + size && isdigit(tar[k])) rlen = rlen * 10 + (size_t)(tar[k++] - '0');
                if (rlen == 0 || rlen > data + size - q || k >= q + rlen || tar[k] != ' ') break;
                if (q + rlen - k - 1 > 6 && !memcmp(tar + k + 1, "path=", 5)) {
                    size_t vlen = q + rlen - (k + 6) - 1;              /* without the closing newline */
                    if (vlen < sizeof longname) { memcpy(longname, tar + k + 6, vlen); longname[vlen] = '\0'; }
                }
                q += rlen;
            }
        } else if (type == 'L') {                               /* GNU: the next entry's long name */
            size_t n = size < sizeof longname ? size : sizeof longname - 1;
            memcpy(longname, tar + data, n);
            longname[n] = '\0';
        } else if ((type == '0' || type == '\0') && !strcmp(entry, name)) {
            uint8_t *out = (uint8_t *)malloc(size ? size : 1);
            if (!out) return NULL;
            memcpy(out, tar + data, size);
            *out_len = size;
            return out;
        }
        p = next;
    }
    return NULL;
}

/* ---- files (UTF-8 paths; wide-character calls on Windows) ---- */

static int find_exe(void) {   /* main thread */
    if (g_exe[0]) return 1;
#if defined(_WIN32)
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n == 0 || n >= 1024 || WideCharToMultiByte(CP_UTF8, 0, w, -1, g_exe, sizeof g_exe, NULL, NULL) <= 0) g_exe[0] = '\0';
#elif !defined(__ANDROID__)
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof g_exe - 1);
    g_exe[n > 0 ? n : 0] = '\0';
#endif
    return g_exe[0] != '\0';
}

#ifdef _WIN32
static int wide(const char *s, wchar_t *w, int cap) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0; }
static int f_remove(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && DeleteFileW(w); }
static int f_exists(const char *p) { wchar_t w[1100]; return wide(p, w, 1100) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES; }
static int f_rename(const char *from, const char *to, int replace) {
    wchar_t a[1100], b[1100];
    return wide(from, a, 1100) && wide(to, b, 1100) && MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
}
/* name.new over name, a note of the program's folder: a read-only mark on the old one does not stop it */
static int f_replace(const char *from, const char *to) {
    wchar_t w[1100];
    DWORD a = wide(to, w, 1100) ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY))
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return f_rename(from, to, 1);
}
/* the whole file, on the disk before this returns (program: an executable; no such mark on Windows) */
static int f_write(const char *path, const uint8_t *data, size_t len, int program) {
    wchar_t w[1100];
    HANDLE h;
    int ok = 1;
    (void)program;
    if (!wide(path, w, 1100)) return 0;
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    for (size_t done = 0; ok && done < len;) {
        DWORD n = 0, chunk = len - done > (1u << 20) ? (1u << 20) : (DWORD)(len - done);
        ok = WriteFile(h, data + done, chunk, &n, NULL) && n > 0;
        done += n;
    }
    ok = ok && FlushFileBuffers(h);
    if (!CloseHandle(h)) ok = 0;
    if (!ok) DeleteFileW(w);
    return ok;
}
#else
static int f_remove(const char *p) { return unlink(p) == 0; }
static int f_exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }
static int f_replace(const char *from, const char *to) { return rename(from, to) == 0; }
/* the whole file, on the disk before this returns (program: executable, rwxr-xr-x; else rw-r--r--) */
static int f_write(const char *path, const uint8_t *data, size_t len, int program) {
    mode_t mode = program ? 0755 : 0644;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode), ok = fd >= 0;
    for (size_t done = 0; ok && done < len;) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        if (ok) done += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0 && fchmod(fd, mode) == 0;
    if (fd >= 0 && close(fd) != 0) ok = 0;
    if (!ok) unlink(path);
    return ok;
}
#endif

/* where the name of the file in path starts (after its folder's separator; Windows takes / as one too) */
static const char *name_in(const char *path) {
    const char *slash = strrchr(path, SEP[0]);
#ifdef _WIN32
    const char *fwd = strrchr(path, '/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
#endif
    return slash ? slash + 1 : path;
}

/* the folder of path, with the separator */
static void folder_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%.*s", (int)(name_in(path) - path), path);
}

/* ---- Android: the system's package installer, through VSSMBActivity ---- */

#ifdef __ANDROID__
static jclass g_act_class;
static jmethodID g_install_method;
static char g_apk[1200];             /* the verified download, kept for another try until it is installed */
static char g_apk_hex[65], g_apk_version[64];

static void android_init(void) {     /* main thread: a worker thread cannot look up the app's classes */
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    const char *dir = SDL_AndroidGetInternalStoragePath();
    if (dir) snprintf(g_apk, sizeof g_apk, "%s/vssmb-update.apk", dir);
    if (!env || g_act_class) return;
    jclass cls = (*env)->FindClass(env, "org/vssmb/nativeport/VSSMBActivity");
    if (!cls) { (*env)->ExceptionClear(env); return; }
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "installUpdate", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!m) { (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, cls); return; }
    g_act_class = (jclass)(*env)->NewGlobalRef(env, cls);
    g_install_method = m;
    (*env)->DeleteLocalRef(env, cls);
}

/* hands the APK to the package installer: "" once Android asks the player to confirm, "permission"
   when it first needs the player to allow this app to install updates, else what went wrong */
static void android_install(const char *path, char *result, size_t cap) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    snprintf(result, cap, "the package installer could not be reached");
    if (!env || !g_act_class) return;
    jstring jpath = (*env)->NewStringUTF(env, path);
    jstring res = (jstring)(*env)->CallStaticObjectMethod(env, g_act_class, g_install_method, jpath);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); res = NULL; }
    if (res) {
        const char *s = (*env)->GetStringUTFChars(env, res, NULL);
        if (s) { snprintf(result, cap, "%s", s); (*env)->ReleaseStringUTFChars(env, res, s); }
        (*env)->DeleteLocalRef(env, res);
    }
    (*env)->DeleteLocalRef(env, jpath);
}

/* VSSMBActivity: what became of the installation the player was asked to confirm. Android closes
   the game before it installs an update, so success is normally not seen here. */
JNIEXPORT void JNICALL Java_org_vssmb_nativeport_VSSMBActivity_nativeUpdateStatus(JNIEnv *env, jclass cls, jint status, jstring message) {
    const char *m = message ? (*env)->GetStringUTFChars(env, message, NULL) : NULL;
    char latest[64];
    (void)cls;
    lock();
    snprintf(latest, sizeof latest, "%s", g_info.latest);
    int waiting = g_info.state == UPD_CONFIRMING;
    unlock();
    if (waiting) {
        if (status == 0) set_state(UPD_INSTALLED, "Version %s is installed. Open the game again to use it.", latest);
        else if (status == 3) set_state(UPD_AVAILABLE, "The update to version %s was cancelled. Press Update now to try again.", latest);
        else set_state(UPD_FAILED, "Android did not install version %s: %s.", latest, m && m[0] ? m : "the installation failed");
    }
    if (m) (*env)->ReleaseStringUTFChars(env, message, m);
}
#endif

/* ---- check ---- */

/* the address of one of this release's own downloads (github.com/<repo>/releases/download/<tag>/...) */
static int release_download(const char *url, const char *tag) {
    char prefix[400];
    int n = snprintf(prefix, sizeof prefix, "https://github.com/%s/releases/download/%s/", VSSMB_REPO, tag);
    return n > 0 && (size_t)n < sizeof prefix && same_text_nocase(url, prefix, (size_t)n) && !strchr(url + n, '/');
}

/* the program's folder can take the new version (desktop): <program>.new can be made there */
static int folder_writable(void) {
#ifdef __ANDROID__
    return 1;
#else
    char probe[1100];
    snprintf(probe, sizeof probe, "%s.new", g_exe);
    if (!g_exe[0] || !f_write(probe, (const uint8_t *)"", 0, 0)) return 0;
    f_remove(probe);
    return 1;
#endif
}

/* the day GitHub's published_at ("2026-11-09T02:26:53Z", UTC) was where the player is: 2026-11-08 in
   Utah. Its first 10 characters (the day in UTC) when it cannot be read. */
static void local_day(const char *utc, char *out, size_t cap) {
    int y, mo, d, h, mi, s;
    struct tm tm;
    snprintf(out, cap, "%.10s", utc);
    if (sscanf(utc, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6 || y < 1970 || mo < 1 || mo > 12 ||
        d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
        return;
    /* days since 1970-01-01 (the civil-calendar count, so that no time zone is involved) */
    int yy = mo <= 2 ? y - 1 : y;
    long long era = yy / 400, yoe = yy - era * 400, doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    time_t t = (time_t)(days * 86400 + h * 3600 + mi * 60 + s);
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return;
#else
    if (!localtime_r(&t, &tm)) return;
#endif
    strftime(out, cap, "%Y-%m-%d", &tm);
}

static void check(void) {
    char url[320], tag[64] = "", page[256] = "", published[40] = "", date[40], asset[512] = "", sums[512] = "", digest[80] = "";
    char name[128], state[24];
    char dir[1100];
    const char *pkg = package_name(), *mine = updater_version();
    long long size = -1;
    http_request_t rq;
    http_response_t rs;
    if (!VSSMB_REPO[0]) {
        set_state(UPD_FAILED, "This build does not know where its releases are published (it was built without a GitHub remote).");
        return;
    }
    snprintf(url, sizeof url, "https://api.github.com/repos/%s/releases/latest", VSSMB_REPO);
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.accept = "application/vnd.github+json";
    rq.user_agent = user_agent();
    rq.timeout_ms = TIMEOUT_MS;
    rq.max_len = (size_t)4 << 20;
    if (!http_request(&rq, &rs)) { set_state(UPD_FAILED, "Could not reach GitHub: %s.", rs.error); return; }
    if (rs.status != 200) {
        free(rs.body);
        if (rs.status == 404) set_state(UPD_CURRENT, "GitHub lists no published release of this game yet.");
        else if (rs.status == 403 || rs.status == 429) set_state(UPD_FAILED, "GitHub is turning away requests from this network for now; try again later.");
        else set_state(UPD_FAILED, "GitHub answered with status %d.", rs.status);
        return;
    }
    const char *b = rs.body, *e = rs.body + rs.len, *list = member(b, e, "assets");
    string_value(member(b, e, "tag_name"), e, tag, sizeof tag);
    string_value(member(b, e, "html_url"), e, page, sizeof page);
    string_value(member(b, e, "published_at"), e, published, sizeof published);
    local_day(published, date, sizeof date);
    for (const char *p = list && *list == '[' ? ws(list + 1, e) : e; p < e && *p == '{';) {
        const char *sz = member(p, e, "size");
        string_value(member(p, e, "name"), e, name, sizeof name);
        string_value(member(p, e, "state"), e, state, sizeof state);
        if (!state[0] || !strcmp(state, "uploaded")) {
            if (pkg[0] && !strcmp(name, pkg)) {
                string_value(member(p, e, "browser_download_url"), e, asset, sizeof asset);
                string_value(member(p, e, "digest"), e, digest, sizeof digest);
                if (sz && isdigit((unsigned char)*sz)) size = strtoll(sz, NULL, 10);
            } else if (!strcmp(name, SUMS_NAME)) {
                string_value(member(p, e, "browser_download_url"), e, sums, sizeof sums);
            }
        }
        p = ws(skip_value(p, e), e);
        p = p < e && *p == ',' ? ws(p + 1, e) : e;
    }
    free(rs.body);
    if (!tag[0]) { set_state(UPD_FAILED, "GitHub's answer could not be read."); return; }

    const char *latest = (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag;
    int cmp = compare_versions(tag, mine);
    int have_pkg = asset[0] && release_download(asset, tag) && size > 0 && (size_t)size <= MAX_PACKAGE;
    int have_sum = !strncmp(digest, "sha256:", 7) || (sums[0] && release_download(sums, tag));
    int writable = cmp > 0 && have_pkg && have_sum && folder_writable();
    lock();
    snprintf(g_info.latest, sizeof g_info.latest, "%s", latest);
    snprintf(g_info.page, sizeof g_info.page, "%s", page);
    snprintf(g_tag, sizeof g_tag, "%s", tag);
    snprintf(g_url, sizeof g_url, "%s", have_pkg ? asset : "");
    snprintf(g_sums_url, sizeof g_sums_url, "%s", sums[0] && release_download(sums, tag) ? sums : "");
    snprintf(g_digest, sizeof g_digest, "%s", digest);
    g_size = size;
    g_info.can_install = writable;
    unlock();
    folder_of(g_exe, dir, sizeof dir);
    if (cmp < 0) set_state(UPD_CURRENT, "This build (%s) is newer than the latest release (%s).", mine, latest);
    else if (cmp == 0) set_state(UPD_CURRENT, "You have the newest version (%s).", latest);
    else if (!have_pkg) set_state(UPD_AVAILABLE, "Version %s is out (published %s), but it has no package for this system (%s): see %s",
                                  latest, date, pkg[0] ? pkg : updater_edition(), page);
    else if (!have_sum) set_state(UPD_AVAILABLE, "Version %s is out (published %s), but the release gives no SHA-256 checksum for "
                                  "%s, so it is not installed from here: see %s", latest, date, pkg, page);
    else if (!writable) set_state(UPD_AVAILABLE, "Version %s is out (published %s), but this program's folder (%.120s) cannot be "
                                  "written: download it from %s", latest, date, dir, page);
#ifdef __ANDROID__
    else set_state(UPD_AVAILABLE, "Version %s is out (published %s). Update now downloads %.1f MB from GitHub and checks it; "
                   "Android then asks you to confirm and closes the game while it installs the update (settings and saves stay).",
                   latest, date, size / 1048576.0);
#else
    else set_state(UPD_AVAILABLE, "Version %s is out (published %s). Update now downloads %.1f MB from GitHub, checks it and "
                   "replaces this program (with its " DOCS_TEXT "); settings, saves and the ROM stay as they are.",
                   latest, date, size / 1048576.0);
#endif
}

/* ---- download and verify ---- */

static int on_progress(void *ctx, size_t received, long long total) {
    (void)ctx;
    SDL_AtomicSet(&g_got, (int)received);
    if (total > 0) SDL_AtomicSet(&g_total, (int)total);
    return SDL_AtomicGet(&g_cancel);
}

static int UPD_PRINTF(3, 4) failed(char *why, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
    return 0;
}

/* worker thread: the package, of the listed size and SHA-256 (hex gets it); a malloc'd buffer or NULL */
static uint8_t *download(size_t *len, char hex[65], char *why, size_t cap) {
    char url[512], sums[512], digest[80], expect[65];
    long long size;
    http_request_t rq;
    http_response_t rs, rsums;
    int checked = 0;

    lock();
    snprintf(url, sizeof url, "%s", g_url);
    snprintf(sums, sizeof sums, "%s", g_sums_url);
    snprintf(digest, sizeof digest, "%s", g_digest);
    size = g_size;
    unlock();
    if (!url[0]) { failed(why, cap, "nothing to install"); return NULL; }

    memset(&rq, 0, sizeof rq);
    rq.user_agent = user_agent();
    rq.timeout_ms = TIMEOUT_MS;
    if (sums[0]) {                           /* the small file first: a package is not downloaded for nothing */
        rq.url = sums;
        rq.max_len = (size_t)64 << 10;
        if (!http_request(&rq, &rsums)) { failed(why, cap, "the checksum file could not be downloaded: %s", rsums.error); return NULL; }
        if (rsums.status != 200) {
            free(rsums.body);
            failed(why, cap, "the checksum file could not be downloaded (GitHub answered with status %d)", rsums.status);
            return NULL;
        }
        int listed = sums_lookup(rsums.body, rsums.len, package_name(), expect);
        free(rsums.body);
        if (!listed) expect[0] = '\0';
    } else {
        expect[0] = '\0';
    }

    rq.url = url;
    rq.max_len = (size_t)size;
    rq.progress = on_progress;
    if (!http_request(&rq, &rs)) { failed(why, cap, "the download failed: %s", rs.error); return NULL; }
    SDL_AtomicSet(&g_got, -1);
    if (rs.status != 200) { free(rs.body); failed(why, cap, "the download failed (GitHub answered with status %d)", rs.status); return NULL; }
    if ((long long)rs.len != size) { free(rs.body); failed(why, cap, "the download was incomplete"); return NULL; }
    sha256_hex((const uint8_t *)rs.body, rs.len, hex);
    if (!strncmp(digest, "sha256:", 7)) {
        if (!same_text_nocase(hex, digest + 7, 65)) { free(rs.body); failed(why, cap, "the download does not match the SHA-256 GitHub lists for it"); return NULL; }
        checked = 1;
    }
    if (expect[0]) {
        if (strcmp(hex, expect)) { free(rs.body); failed(why, cap, "the download does not match the release's " SUMS_NAME); return NULL; }
        checked = 1;
    }
    if (!checked) { free(rs.body); failed(why, cap, "the release gives no SHA-256 for %s", package_name()); return NULL; }
    *len = rs.len;
    return (uint8_t *)rs.body;
}

/* ---- install ---- */

#ifndef __ANDROID__
static int g_kept_old;               /* the last installation kept the previous program as <name>.old */

/* a malloc'd copy of the package's file PACKAGE_DIR/name (arc: the zip on Windows, the unpacked tar
   elsewhere), or NULL */
static uint8_t *package_file(const uint8_t *arc, size_t len, const char *name, size_t *out_len) {
    char entry[256];
    snprintf(entry, sizeof entry, "%s/%s", PACKAGE_DIR, name);
#ifdef _WIN32
    return zip_entry(arc, len, entry, out_len);
#else
    return tar_entry(arc, len, entry, out_len);
#endif
}

/* worker thread: the new program into this one's place, as the file comment describes, with the notes
   the package has for it (doc_names). Each new file is first written in full beside the one it replaces;
   then the notes take their places, then the program. Nothing is replaced when a file cannot be written,
   and the program stays as it was when anything fails before its own swap. */
static int install(char *why, size_t cap) {
    char hex[65], dir[sizeof g_exe], newbin[1100], backup[1100], doc_path[N_DOCS][1100], doc_new[N_DOCS][1100];
    uint8_t *pkg, *arc, *bin = NULL, *doc[N_DOCS];
    size_t len = 0, arclen = 0, binlen = 0, doclen[N_DOCS], k;
    int ok = 0, written, have[N_DOCS];
    g_kept_old = 0;
    pkg = download(&len, hex, why, cap);
    if (!pkg) return 0;
#ifdef _WIN32
    arc = pkg;                                   /* the zip is read where it is */
    arclen = len;
#else
    arc = gunzip(pkg, len, &arclen);             /* the tar */
    free(pkg);
#endif
    for (k = 0; k < N_DOCS; k++) { doc[k] = NULL; doclen[k] = 0; }
    if (arc) {
        bin = package_file(arc, arclen, BINARY, &binlen);
        for (k = 0; k < N_DOCS; k++) doc[k] = package_file(arc, arclen, doc_names[k], &doclen[k]);
        free(arc);
    }
#ifdef _WIN32
    if (bin && (binlen < 2 || bin[0] != 'M' || bin[1] != 'Z')) { free(bin); bin = NULL; }
#else
    if (bin && (binlen < 4 || memcmp(bin, "\177ELF", 4))) { free(bin); bin = NULL; }
#endif
    if (!bin) {
        for (k = 0; k < N_DOCS; k++) free(doc[k]);
        return failed(why, cap, "the package holds no usable %s", BINARY);
    }

    folder_of(g_exe, dir, sizeof dir);
    snprintf(newbin, sizeof newbin, "%s.new", g_exe);
    snprintf(backup, sizeof backup, "%s.old", g_exe);
    written = f_write(newbin, bin, binlen, 1);
    free(bin);
    for (k = 0; k < N_DOCS; k++) {
        snprintf(doc_path[k], sizeof doc_path[k], "%s%s", dir, doc_names[k]);
        snprintf(doc_new[k], sizeof doc_new[k], "%s%s.new", dir, doc_names[k]);
        have[k] = doc[k] != NULL;               /* a note the package does not have stays as it is */
        if (written && have[k]) written = f_write(doc_new[k], doc[k], doclen[k], 0);
        free(doc[k]);
    }

    if (g_swap) SDL_LockMutex(g_swap);
    if (!written) {
        failed(why, cap, "the new version could not be written beside this one");
    } else if (SDL_AtomicGet(&g_cancel)) {
        failed(why, cap, "the program is closing");
    } else {
#ifdef _WIN32
        /* Windows cannot replace a running program's file, but can rename it */
        f_remove(backup);
        if (f_exists(backup)) failed(why, cap, "the previous version (%s.old) is still running", BINARY);
        else {
            for (k = 0; k < N_DOCS && (!have[k] || f_replace(doc_new[k], doc_path[k])); k++) {}
            if (k < N_DOCS) failed(why, cap, "%s could not be replaced (is it open in another program?)", doc_names[k]);
            else if (!f_rename(g_exe, backup, 0)) failed(why, cap, "the running program could not be renamed");
            else if (!f_rename(newbin, g_exe, 0)) { f_rename(backup, g_exe, 0); failed(why, cap, "the new version could not take the program's place"); }
            else ok = g_kept_old = 1;
        }
#else
        /* the old version keeps a second name where the drive allows a hard link; the rename replaces
           the program in one step */
        int linked;
        f_remove(backup);
        linked = link(g_exe, backup) == 0;
        for (k = 0; k < N_DOCS && (!have[k] || f_replace(doc_new[k], doc_path[k])); k++) {}
        if (k < N_DOCS) failed(why, cap, "%s could not be replaced (%s)", doc_names[k], strerror(errno));
        else if (rename(newbin, g_exe) != 0) failed(why, cap, "the new version could not take the program's place (%s)", strerror(errno));
        else {
            int fd = open(dir[0] ? dir : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd >= 0) { fsync(fd); close(fd); }
            ok = 1;
            g_kept_old = linked;
        }
        if (!ok && linked) f_remove(backup);
#endif
    }
    if (!ok) {                                   /* the new files that did not take their places */
        f_remove(newbin);
        for (k = 0; k < N_DOCS; k++) if (have[k]) f_remove(doc_new[k]);
    }
    if (g_swap) SDL_UnlockMutex(g_swap);
    return ok;
}
#else
/* worker thread: download and verify, then the package installer takes over (UPD_CONFIRMING) */
static int install(char *why, size_t cap) {
    char hex[65], latest[64], result[300];
    size_t len = 0;
    uint8_t *apk = NULL;
    lock();
    snprintf(latest, sizeof latest, "%s", g_info.latest);
    unlock();
    if (!g_apk[0]) return failed(why, cap, "the app's storage was not found");
    /* a download verified before (when Android first asked for permission) is used again if unchanged */
    if (g_apk_hex[0] && !strcmp(g_apk_version, latest)) {
        FILE *f = fopen(g_apk, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && (size_t)n <= MAX_PACKAGE && (apk = (uint8_t *)malloc((size_t)n)) != NULL && fread(apk, 1, (size_t)n, f) == (size_t)n) {
                len = (size_t)n;
                sha256_hex(apk, len, hex);
                if (strcmp(hex, g_apk_hex)) { free(apk); apk = NULL; }
            } else {
                free(apk);
                apk = NULL;
            }
            fclose(f);
        }
        if (apk) SDL_AtomicSet(&g_got, -1);
    }
    if (!apk) {
        g_apk_hex[0] = '\0';
        apk = download(&len, hex, why, cap);
        if (!apk) return 0;
        if (len < 4 || memcmp(apk, "PK\003\004", 4)) { free(apk); return failed(why, cap, "the download is not an Android package"); }
        FILE *f = fopen(g_apk, "wb");
        int written = f && fwrite(apk, 1, len, f) == len;
        if (f && fclose(f) != 0) written = 0;
        if (!written) { free(apk); remove(g_apk); return failed(why, cap, "the download could not be saved in the app's storage"); }
        snprintf(g_apk_hex, sizeof g_apk_hex, "%s", hex);
        snprintf(g_apk_version, sizeof g_apk_version, "%s", latest);
    }
    free(apk);
    set_state(UPD_CONFIRMING, "Android is asking you to confirm the update to version %s. The game then closes while it is "
              "installed; open it again afterwards.", latest);
    android_install(g_apk, result, sizeof result);
    if (!result[0]) return 1;
    if (!strcmp(result, "permission")) {
        set_state(UPD_AVAILABLE, "Android first needs your permission for this app to install updates: allow it on the "
                  "settings page that opened (Install unknown apps), come back and press Update now again.");
        return 1;
    }
    return failed(why, cap, "%s", result);
}
#endif

static int SDLCALL check_worker(void *arg) {
    (void)arg;
    check();
    return 0;
}

static int SDLCALL install_worker(void *arg) {
    char why[400], latest[64];
    (void)arg;
    lock();
    snprintf(latest, sizeof latest, "%s", g_info.latest);
    unlock();
    if (!install(why, sizeof why))
        set_state(UPD_FAILED, "Version %s was not installed: %s.", latest, why);
#ifndef __ANDROID__
#  define INSTALLED "Version %s is installed. Restart now to use it (a game in progress ends), or it starts the next time you open the game."
    else if (g_kept_old)
        set_state(UPD_INSTALLED, INSTALLED " The previous version is kept as %s.old.", latest, g_exe[0] ? name_in(g_exe) : BINARY);
    else
        set_state(UPD_INSTALLED, INSTALLED, latest);
#  undef INSTALLED
#endif
    return 0;
}

static void start(int (SDLCALL *fn)(void *), int state, const char *text) {
    SDL_Thread *t;
    set_state(state, "%s", text);
    t = SDL_CreateThread(fn, "update", NULL);
    if (t) SDL_DetachThread(t);
    else set_state(UPD_FAILED, "Could not start a background thread.");
}

void updater_startup(void) {
    char path[1100];
    if (!g_lock) g_lock = SDL_CreateMutex();
    if (!g_swap) g_swap = SDL_CreateMutex();
    find_exe();
#ifdef __ANDROID__
    android_init();
    if (g_apk[0]) remove(g_apk);     /* an update that was installed, or never confirmed */
#else
    if (g_exe[0]) {                  /* the new files of an update that did not finish */
        char dir[sizeof g_exe];
        snprintf(path, sizeof path, "%s.new", g_exe);
        if (f_exists(path)) f_remove(path);
        folder_of(g_exe, dir, sizeof dir);
        for (size_t k = 0; k < N_DOCS; k++) {
            snprintf(path, sizeof path, "%s%s.new", dir, doc_names[k]);
            if (f_exists(path)) f_remove(path);
        }
    }
#endif
    (void)path;
}

void updater_check(void) {
    const char *why;
    lock();
    int busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    unlock();
    if (busy) return;
    if (!http_init(&why)) { set_state(UPD_FAILED, "Could not check: %s.", why); return; }
    find_exe();
    start(check_worker, UPD_CHECKING, "Asking GitHub for the newest version...");
}

void updater_install(void) {
    update_info cur;
    char text[120];
    updater_poll(&cur);
    if (!(cur.state == UPD_AVAILABLE || cur.state == UPD_CONFIRMING) || !cur.can_install) return;
    SDL_AtomicSet(&g_got, 0);
    SDL_AtomicSet(&g_total, 0);
    snprintf(text, sizeof text, "Downloading version %s...", cur.latest);
    start(install_worker, UPD_DOWNLOADING, text);
}

void updater_poll(update_info *out) {
    lock();
    *out = g_info;
    unlock();
    if (out->state == UPD_DOWNLOADING) {
        int got = SDL_AtomicGet(&g_got), total = SDL_AtomicGet(&g_total);
        if (got < 0) snprintf(out->text, sizeof out->text, "Checking and installing version %s...", out->latest);
        else if (total > 0) snprintf(out->text, sizeof out->text, "Downloading version %s: %.1f of %.1f MB...", out->latest,
                                     got / 1048576.0, total / 1048576.0);
    }
}

void updater_shutdown(void) {
    SDL_AtomicSet(&g_cancel, 1);
    if (g_swap) SDL_LockMutex(g_swap);   /* kept: no swap starts after this */
}

/* ---- restart ---- */

#ifdef _WIN32
/* one argument for CommandLineToArgvW / the C runtime's parser */
static void quote_arg(char *out, size_t cap, size_t *n, const char *arg) {
#  define PUT(c) do { if (*n + 1 < cap) out[(*n)++] = (c); } while (0)
    if (*n) PUT(' ');
    PUT('"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        if (!*p) { while (slashes--) { PUT('\\'); PUT('\\'); } break; }
        if (*p == '"') { while (slashes--) { PUT('\\'); PUT('\\'); } PUT('\\'); PUT('"'); }
        else { while (slashes--) PUT('\\'); PUT(*p); }
    }
    PUT('"');
    out[*n] = '\0';
#  undef PUT
}
#endif

int updater_restart(char **argv) {
    if (!find_exe()) return 0;
    fflush(NULL);
#if defined(_WIN32)
    char line[4096];
    size_t n = 0;
    wchar_t wexe[1100], wline[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    quote_arg(line, sizeof line, &n, g_exe);
    for (int i = 1; argv && argv[i]; i++) quote_arg(line, sizeof line, &n, argv[i]);
    if (!wide(g_exe, wexe, 1100) || !wide(line, wline, 4096)) return 0;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(wexe, wline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#elif defined(__ANDROID__)
    (void)argv;
    return 0;
#else
    execv(g_exe, argv);
    fprintf(stderr, "restart: %s: %s\n", g_exe, strerror(errno));
    return 0;
#endif
}

int updater_selftest(const char *pretend, char **then) {
    update_info info;
    char why[400];
    const char *reason;
    if (pretend) snprintf(g_pretend, sizeof g_pretend, "%s", pretend);
    updater_startup();
    printf("update: this build %s (%s), package %s, repository %s\n", updater_version(), updater_edition(),
           package_name()[0] ? package_name() : "-", VSSMB_REPO[0] ? VSSMB_REPO : "-");
    if (!http_init(&reason)) { printf("update: %s\n", reason); return 1; }
    check();
    updater_poll(&info);
    printf("update: %s\n", info.text);
    if (!then) return info.state == UPD_FAILED;
    if (info.state != UPD_AVAILABLE || !info.can_install) return 1;
#ifdef __ANDROID__
    (void)why;
    return 1;
#else
    if (!install(why, sizeof why)) { printf("update: not installed: %s\n", why); return 1; }
    printf("update: installed %s as %s\n", info.latest, g_exe);
    fflush(stdout);
    return updater_restart(then) ? 0 : 1;
#endif
}
