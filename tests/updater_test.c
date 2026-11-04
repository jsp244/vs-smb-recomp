/*
 * updater_test.c - offline checks of the updater's own parts (frontend/updater.c is included,
 * so its internal functions can be called): version order, SHA-256, SHA256SUMS lines, which
 * addresses count as the release's own downloads, taking the program out of packages made the
 * way `make dist` makes them, and whole checks and installations against a stand-in GitHub
 * (the HTTP calls are stubs serving a release made up here: nothing goes online), including
 * every reason to refuse a download.
 * Build and run: tests/run_updater_test.sh
 */
#define SDL_MAIN_HANDLED
#define VSSMB_REPO "owner/vs-smb-recomp"
#define VSSMB_VERSION "v1.1.0"
#ifdef _WIN32
#  define VSSMB_PACKAGE "vssmb-win64.zip"
#else
#  define VSSMB_PACKAGE "vssmb-linux-x86_64.tar.gz"
#endif
#include "../frontend/updater.c"

/* the stand-in GitHub: the release's JSON, the package and SHA256SUMS at their addresses */
#define RELEASE "https://github.com/owner/vs-smb-recomp/releases/download/v1.2.0/"
static char fake_json[4096], fake_sums[512];
static uint8_t *fake_pkg;
static size_t fake_pkg_len;
static int requests;

int http_init(const char **why) { if (why) *why = ""; return 1; }
int http_request(const http_request_t *rq, http_response_t *rs) {
    const void *body = NULL;
    size_t len = 0;
    memset(rs, 0, sizeof *rs);
    requests++;
    if (!strcmp(rq->url, "https://api.github.com/repos/owner/vs-smb-recomp/releases/latest")) { body = fake_json; len = strlen(fake_json); }
    else if (!strcmp(rq->url, RELEASE VSSMB_PACKAGE)) { body = fake_pkg; len = fake_pkg_len; }
    else if (!strcmp(rq->url, RELEASE "SHA256SUMS") && fake_sums[0]) { body = fake_sums; len = strlen(fake_sums); }
    rs->status = body ? 200 : 404;
    rs->body = (char *)malloc(len + 1);
    if (body) memcpy(rs->body, body, len);
    rs->body[len] = '\0';
    rs->len = len;
    return 1;
}

/* a release v1.2.0 with the package (listed as size bytes, with GitHub's digest when there is
   one) and, when sums is given, a SHA256SUMS file */
static void fake_release(const char *asset_url, long long size, const char *digest, const char *sums) {
    int n = snprintf(fake_json, sizeof fake_json,
        "{\"url\":\"https://api.github.com/repos/owner/vs-smb-recomp/releases/1\",\"tag_name\":\"v1.2.0\",\"draft\":false,"
        "\"html_url\":\"https://github.com/owner/vs-smb-recomp/releases/tag/v1.2.0\",\"published_at\":\"2026-11-05T10:00:00Z\","
        "\"body\":\"What's new: \\\"quotes\\\", braces { } and [brackets]\",\"assets\":[");
    if (sums) n += snprintf(fake_json + n, sizeof fake_json - n,
        "{\"name\":\"SHA256SUMS\",\"size\":%d,\"state\":\"uploaded\",\"digest\":null,\"uploader\":{\"login\":\"x\"},"
        "\"browser_download_url\":\"" RELEASE "SHA256SUMS\"},", (int)strlen(sums));
    snprintf(fake_json + n, sizeof fake_json - n,
        "{\"name\":\"" VSSMB_PACKAGE "\",\"size\":%lld,\"state\":\"uploaded\",\"digest\":%s%s%s,"
        "\"browser_download_url\":\"%s\"}]}", size, digest ? "\"" : "", digest ? digest : "null", digest ? "\"" : "", asset_url);
    snprintf(fake_sums, sizeof fake_sums, "%s", sums ? sums : "");
}

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok:   %s\n", msg); } while (0)

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf = NULL;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 0 && (buf = (uint8_t *)malloc((size_t)n + 1)) != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

static int same_as_file(const uint8_t *data, size_t len, const char *path) {
    size_t flen = 0;
    uint8_t *f = read_all(path, &flen);
    int same = f && flen == len && !memcmp(f, data, len);
    free(f);
    return same;
}

/* the stand-in program before each case: "old version", with no backup beside it */
static void reset_program(const char *exe) {
    char p[1300];
    FILE *f = fopen(exe, "wb");
    if (f) { fputs("old version", f); fclose(f); }
    snprintf(p, sizeof p, "%s.old", exe);
    remove(p);
}

static int program_is(const char *exe, const char *ref) {
    size_t len = 0;
    uint8_t *data = read_all(ref, &len);
    int same = data && same_as_file(data, len, exe);
    free(data);
    return same;
}

static int no_leftover(const char *exe) {
    char p[1300];
    snprintf(p, sizeof p, "%s.new", exe);
    return !f_exists(p);
}

static int backup_is_old(const char *exe) {
    char p[1300];
    snprintf(p, sizeof p, "%s.old", exe);
    return same_as_file((const uint8_t *)"old version", 11, p);
}

static int untouched(const char *exe) {
    char p[1300];
    snprintf(p, sizeof p, "%s.old", exe);
    return same_as_file((const uint8_t *)"old version", 11, exe) && !f_exists(p) && no_leftover(exe);
}

int main(int argc, char **argv) {
    char hex[65], path[1200];
    SDL_SetMainReady();

    CHECK(compare_versions("1.0.1", "1.0.0") > 0, "1.0.1 is newer than 1.0.0");
    CHECK(compare_versions("v1.0.0", "1.0.0") == 0, "v1.0.0 is 1.0.0");
    CHECK(compare_versions("v1.10.0", "1.9.9") > 0, "1.10.0 is newer than 1.9.9 (numbers, not text)");
    CHECK(compare_versions("1.0.0-3-gabc1234", "v1.0.0") > 0, "3 commits after the tag are newer than the tag");
    CHECK(compare_versions("1.0.0-dirty", "v1.0.0") == 0, "an edited checkout of the tag is the tag");
    CHECK(compare_versions("dev", "v0.0.1") < 0, "a build without a version is older than any release");
    CHECK(compare_versions("v2", "1.9.9") > 0, "2 is newer than 1.9.9");

    sha256_hex((const uint8_t *)"abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of \"abc\"");
    sha256_hex((const uint8_t *)"", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing");
    {
        const char *s = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes: the padding takes a second block */
        sha256_hex((const uint8_t *)s, strlen(s), hex);
        CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 of the 56-byte test string");
        uint8_t *a = (uint8_t *)malloc(1000000);
        memset(a, 'a', 1000000);
        sha256_hex(a, 1000000, hex);
        free(a);
        CHECK(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million a's");
    }

    {
        const char *sums =
            "1111111111111111111111111111111111111111111111111111111111111111  vssmb-win64.zip\r\n"
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA *vssmb-steamdeck.tar.gz\n"
            "2222222222222222222222222222222222222222222222222222222222222222  vssmb-linux-x86_64.tar.gz.old\n"
            "33333333  vssmb-linux-x86_64.tar.gz\n";
        CHECK(sums_lookup(sums, strlen(sums), "vssmb-win64.zip", hex) && hex[0] == '1' && strlen(hex) == 64, "SHA256SUMS: a CRLF line");
        CHECK(sums_lookup(sums, strlen(sums), "vssmb-steamdeck.tar.gz", hex) && !strcmp(hex, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              "SHA256SUMS: binary mark, upper case");
        CHECK(!sums_lookup(sums, strlen(sums), "vssmb-linux-x86_64.tar.gz", hex), "SHA256SUMS: a short hash or a longer name is no match");
        CHECK(!sums_lookup(sums, strlen(sums), "vssmb-android-phone.apk", hex), "SHA256SUMS: a package it does not list");
    }

    CHECK(release_download("https://github.com/owner/vs-smb-recomp/releases/download/v1.2.0/vssmb-win64.zip", "v1.2.0"), "the release's own download");
    CHECK(release_download("https://github.com/Owner/VS-SMB-recomp/releases/download/v1.2.0/vssmb-win64.zip", "v1.2.0"), "the repository's name in other letter case");
    CHECK(!release_download("https://github.com/owner/vs-smb-recomp/releases/download/v1.1.0/vssmb-win64.zip", "v1.2.0"), "not another release's download");
    CHECK(!release_download("https://github.com/other/vs-smb-recomp/releases/download/v1.2.0/vssmb-win64.zip", "v1.2.0"), "not another repository's download");
    CHECK(!release_download("http://github.com/owner/vs-smb-recomp/releases/download/v1.2.0/vssmb-win64.zip", "v1.2.0"), "not over http");
    CHECK(!release_download("https://github.com/owner/vs-smb-recomp/releases/download/v1.2.0/x/vssmb-win64.zip", "v1.2.0"), "not a deeper path");
    CHECK(!release_download("https://example.com/owner/vs-smb-recomp/releases/download/v1.2.0/vssmb-win64.zip", "v1.2.0"), "not another site");

    /* packages from tests/run_updater_test.sh: test.zip holding vssmb/vssmb.exe (made from the folder
       <dir>/win) and test.tar.gz holding vssmb/vssmb (from <dir>/linux) */
    if (argc >= 4) {
        size_t len = 0, out = 0, tlen = 0;
        uint8_t *zip = read_all(argv[1], &len), *bin;
        CHECK(zip != NULL, "the zip package can be read");
        bin = zip ? zip_entry(zip, len, "vssmb/vssmb.exe", &out) : NULL;
        snprintf(path, sizeof path, "%s/win/vssmb/vssmb.exe", argv[3]);
        CHECK(bin && same_as_file(bin, out, path), "zip: vssmb/vssmb.exe comes out byte for byte");
        free(bin);
        CHECK(zip && !zip_entry(zip, len, "vssmb/missing.exe", &out), "zip: a file it does not hold");
        if (zip && len > 100) {
            zip[len / 2] ^= 0x55;                             /* a damaged package fails its CRC */
            bin = zip_entry(zip, len, "vssmb/vssmb.exe", &out);
            CHECK(!bin, "zip: a damaged entry is refused");
            free(bin);
        }
        free(zip);

        uint8_t *gz = read_all(argv[2], &len), *tar = gz ? gunzip(gz, len, &tlen) : NULL;
        CHECK(tar != NULL, "tar.gz: gunzip");
        bin = tar ? tar_entry(tar, tlen, "vssmb/vssmb", &out) : NULL;
        snprintf(path, sizeof path, "%s/linux/vssmb/vssmb", argv[3]);
        CHECK(bin && same_as_file(bin, out, path), "tar.gz: vssmb/vssmb comes out byte for byte");
        free(bin);
        bin = tar ? tar_entry(tar, tlen, "vssmb/README.txt", &out) : NULL;
        CHECK(bin && out == 6 && !memcmp(bin, "notes\n", 6), "tar.gz: a second file");
        free(bin);
        CHECK(tar && !tar_entry(tar, tlen, "vssmb", &out), "tar.gz: a folder is not a file");
        free(tar);
        if (gz && len > 100) {
            gz[len / 2] ^= 0x55;
            tar = gunzip(gz, len, &tlen);
            CHECK(!tar, "tar.gz: a damaged package is refused");
            free(tar);
        }
        free(gz);
    } else {
        printf("(no packages given: the archive checks were skipped)\n");
    }

    /* whole checks and installations against the stand-in release: the program is a stand-in in
       <dir>/run, replaced by the one in this system's package */
    if (argc >= 4) {
        char why[400], ref[1200], sums[400], other[400];
        update_info u;
#ifdef _WIN32
        fake_pkg = read_all(argv[1], &fake_pkg_len);
        snprintf(ref, sizeof ref, "%s/win/vssmb/vssmb.exe", argv[3]);
        snprintf(g_exe, sizeof g_exe, "%s/run/vssmb.exe", argv[3]);
#else
        fake_pkg = read_all(argv[2], &fake_pkg_len);
        snprintf(ref, sizeof ref, "%s/linux/vssmb/vssmb", argv[3]);
        snprintf(g_exe, sizeof g_exe, "%s/run/vssmb", argv[3]);
#endif
        if (!fake_pkg) { printf("FAIL: the package for this system cannot be read\n"); return 1; }
        updater_startup();
        sha256_hex(fake_pkg, fake_pkg_len, hex);
        char digest[80], wrong[80];
        snprintf(digest, sizeof digest, "sha256:%s", hex);
        snprintf(wrong, sizeof wrong, "sha256:%s", "0000000000000000000000000000000000000000000000000000000000000000");
        snprintf(sums, sizeof sums, "%s  " VSSMB_PACKAGE "\n%s  vssmb-android-phone.apk\n", hex, hex);
        snprintf(other, sizeof other, "%s  vssmb-android-phone.apk\n", hex);
        const char *bad_sums = "1111111111111111111111111111111111111111111111111111111111111111  " VSSMB_PACKAGE "\n";
        long long size = (long long)fake_pkg_len;

        /* each case: the program as before, a release, a check, and Update now */
#define CASE(url, sz, dig, sm) (reset_program(g_exe), fake_release((url), (sz), (dig), (sm)), check(), updater_poll(&u), \
                                (u.state == UPD_AVAILABLE && u.can_install) ? install(why, sizeof why) : -1)
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, digest, NULL) == 1 && program_is(g_exe, ref) && backup_is_old(g_exe) && no_leftover(g_exe),
              "GitHub's digest: installed, the old program kept as .old");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, NULL, sums) == 1 && program_is(g_exe, ref), "SHA256SUMS alone: installed");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, digest, other) == 1 && program_is(g_exe, ref),
              "SHA256SUMS that does not list the package, with the digest: installed");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, wrong, NULL) == 0 && strstr(why, "SHA-256 GitHub lists") && untouched(g_exe),
              "a wrong digest: refused, nothing changed");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, NULL, bad_sums) == 0 && strstr(why, "SHA256SUMS") && untouched(g_exe),
              "a wrong SHA256SUMS line: refused, nothing changed");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, digest, bad_sums) == 0 && untouched(g_exe), "the digest right but SHA256SUMS wrong: refused");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, NULL, other) == 0 && strstr(why, "no SHA-256") && untouched(g_exe),
              "no checksum for the package anywhere: refused");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size + 1, digest, NULL) == 0 && strstr(why, "incomplete") && untouched(g_exe),
              "a download shorter than GitHub lists: refused");
        CHECK(CASE(RELEASE VSSMB_PACKAGE, size, NULL, NULL) == -1 && u.state == UPD_AVAILABLE && strstr(u.text, "no SHA-256"),
              "a release without any checksum: offered as a download page only");
        CHECK(CASE("https://github.com/owner/vs-smb-recomp/releases/download/v1.1.0/" VSSMB_PACKAGE, size, digest, NULL) == -1 &&
              strstr(u.text, "no package for this system"), "a package of another release: not installed from here");
        CHECK(CASE("https://example.com/" VSSMB_PACKAGE, size, digest, NULL) == -1, "a package from another site: not installed from here");
#undef CASE
        snprintf(g_pretend, sizeof g_pretend, "1.2.0");
        fake_release(RELEASE VSSMB_PACKAGE, size, digest, NULL);
        check();
        updater_poll(&u);
        CHECK(u.state == UPD_CURRENT && strstr(u.text, "newest version (1.2.0)"), "the same version: nothing to install");
        snprintf(g_pretend, sizeof g_pretend, "1.2.0-4-gabcdef1");
        check();
        updater_poll(&u);
        CHECK(u.state == UPD_CURRENT && strstr(u.text, "newer than the latest release"), "a newer build: nothing to install");
        g_pretend[0] = '\0';
        free(fake_pkg);
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
