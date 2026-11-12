/*
 * headless.c - command-line harness: runs the machine without a window,
 * for test ROMs, scripted input, screenshots, traces, coverage files and
 * interpreter-vs-recompiled cosimulation.
 */
#include "nes.h"
#include "png.h"
#include "palette.h"
#include "widescreen.h"
#include "allstars.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* a monotonic clock in milliseconds, for timing single calls (--reset) */
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static FILE *trace_file;
static uint32_t pal[512];
static int watch_addr = -1;

static void trace_hook(void) {
    fprintf(trace_file, "%04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%llu",
            cpu.pc, cpu.a, cpu.x, cpu.y, cpu_get_p(0), cpu.sp, (unsigned long long)cpu.cycles);
    fprintf(trace_file, " B:%02X,%02X", mapper.prg_bank[1], mapper.prg_bank[2]);   /* banks at $A000 / $C000 */
    if (watch_addr >= 0) fprintf(trace_file, " W:%02X", mem_read((uint16_t)watch_addr));
    fputc('\n', trace_file);
}

/* buttons of an input event: bits 0-7 the first controller, 8-15 the second (names with a "2"
   in front), 16-18 the Vs. System's coin slots and service button. On the Vs. System the
   1P / 2P start buttons are the controllers' SELECT lines. */
static uint32_t parse_buttons(const char *s) {
    uint32_t b = 0;
    while (*s) {
        const char *end = s;
        while (*end && *end != '+' && *end != ',') end++;
        size_t n = (size_t)(end - s);
        uint32_t shift = 0;
        if (n > 1 && s[0] == '2' && strncmp(s, "2P", 2) != 0) { shift = 8; s++; n--; }
        #define BTN(name, bit) if (n == strlen(name) && !strncmp(s, name, n)) b |= (uint32_t)(bit) << shift
        BTN("A", 0x01); BTN("B", 0x02); BTN("SELECT", 0x04); BTN("START", 0x08);
        BTN("UP", 0x10); BTN("DOWN", 0x20); BTN("LEFT", 0x40); BTN("RIGHT", 0x80);
        #undef BTN
        if (n == 4 && !strncmp(s, "COIN", 4)) b |= 1u << 16;
        if (n == 5 && !strncmp(s, "COIN2", 5)) b |= 2u << 16;
        if (n == 7 && !strncmp(s, "SERVICE", 7)) b |= 4u << 16;
        s = *end ? end + 1 : end;
    }
    return b;
}

typedef struct { int frame; uint32_t buttons; } input_event_t;
static input_event_t events[4096];
static int n_events;

static void add_event(int frame, uint32_t b) {
    if (n_events < 4096) { events[n_events].frame = frame; events[n_events].buttons = b; n_events++; }
}

static void load_input_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open input script %s\n", path); exit(1); }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int frame; char btn[200];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, "%d %199s", &frame, btn) == 2) add_event(frame, parse_buttons(btn));
    }
    fclose(f);
}

static void parse_keys(const char *spec) {
    char *buf = malloc(strlen(spec) + 1);
    strcpy(buf, spec);
    char *p = buf;
    while (*p) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = 0;
            add_event(atoi(p), parse_buttons(colon + 1));
        }
        if (!semi) break;
        p = semi + 1;
    }
    free(buf);
}

/* --poke: CPU-bus writes before a frame runs (RAM, work RAM), e.g. to start a test in another level */
typedef struct { int frame; uint16_t addr; uint8_t val; } poke_t;
static poke_t pokes[256];
static int n_pokes;

static void parse_pokes(const char *spec) {
    const char *p = spec;
    while (*p && n_pokes < 256) {
        int frame = atoi(p);
        const char *colon = strchr(p, ':');
        if (!colon) break;
        p = colon + 1;
        while (*p && *p != ';' && n_pokes < 256) {
            unsigned addr = (unsigned)strtoul(p, (char **)&p, 16);
            if (*p != '=') break;
            unsigned val = (unsigned)strtoul(p + 1, (char **)&p, 16);
            pokes[n_pokes].frame = frame; pokes[n_pokes].addr = (uint16_t)addr; pokes[n_pokes].val = (uint8_t)val;
            n_pokes++;
            if (*p == ',') p++;
        }
        if (*p == ';') p++;
        else break;
    }
}

/* --reset: the reset button before these frames run, as the frontend's Play > Reset game (between frames) */
static int reset_frames[64];
static int n_resets;

static void parse_resets(const char *spec) {
    for (const char *p = spec; *p && n_resets < 64;) {
        reset_frames[n_resets++] = atoi(p);
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
}

static void fb_to_rgb(uint8_t *rgb) {
    for (int y = 0; y < 240; y++) {
        int emph = (ppu.line_emph[y] >> 5) & 7;
        for (int x = 0; x < 256; x++) {
            uint32_t c = pal[emph * 64 + (ppu.fb[y * 256 + x] & 0x3F)];
            rgb[(y * 256 + x) * 3] = (uint8_t)(c >> 16);
            rgb[(y * 256 + x) * 3 + 1] = (uint8_t)(c >> 8);
            rgb[(y * 256 + x) * 3 + 2] = (uint8_t)c;
        }
    }
}

static uint8_t wide_fb[WS_W * WS_H], wide_emph[WS_H];
static uint32_t as_px[WS_W * WS_H];

/* the remastered picture (allstars.c) as RGB: the PPU's 256x240, or with wide the whole canvas;
   returns 0 (and writes nothing) when it was not drawn */
static int allstars_to_rgb(uint8_t *rgb, int wide) {
    int x0 = wide ? -WS_BAND : 0, w = wide ? WS_W : 256;
    if (!allstars_render(as_px, w, x0, 0, w, 240)) return 0;
    for (int i = 0; i < w * 240; i++) {
        rgb[i * 3] = (uint8_t)(as_px[i] >> 16); rgb[i * 3 + 1] = (uint8_t)(as_px[i] >> 8); rgb[i * 3 + 2] = (uint8_t)as_px[i];
    }
    return 1;
}

/* the whole widescreen canvas (centre = the PPU frame, beside it the level or black) as RGB */
static int wide_to_rgb(uint8_t *rgb) {
    int level = widescreen_render(wide_fb, wide_emph, 0, 0, WS_W, WS_H);
    for (int y = 0; y < WS_H; y++) {
        int emph = (wide_emph[y] >> 5) & 7;
        for (int x = 0; x < WS_W; x++) {
            uint32_t c = pal[emph * 64 + (wide_fb[y * WS_W + x] & 0x3F)];
            rgb[(y * WS_W + x) * 3] = (uint8_t)(c >> 16);
            rgb[(y * WS_W + x) * 3 + 1] = (uint8_t)(c >> 8);
            rgb[(y * WS_W + x) * 3 + 2] = (uint8_t)c;
        }
    }
    return level;
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static uint64_t frame_hash(void) {
    uint64_t h = 0xcbf29ce484222325ULL;
    uint8_t regs[8] = { cpu.a, cpu.x, cpu.y, cpu.sp, (uint8_t)cpu.pc, (uint8_t)(cpu.pc >> 8), cpu_get_p(0), 0 };
    h = fnv1a(h, regs, sizeof regs);
    h = fnv1a(h, &cpu.cycles, sizeof cpu.cycles);
    h = fnv1a(h, ram, sizeof ram);
    h = fnv1a(h, cart.wram, sizeof cart.wram);
    h = fnv1a(h, ppu.fb, sizeof ppu.fb);
    h = fnv1a(h, ppu.oam, sizeof ppu.oam);
    h = fnv1a(h, ppu.palette, sizeof ppu.palette);
    h = fnv1a(h, ppu.vram, sizeof ppu.vram);
    h = fnv1a(h, mapper.prg_bank, sizeof mapper.prg_bank);
    h = fnv1a(h, mapper.chr_bank, sizeof mapper.chr_bank);
    return h;
}

static void write_wav(const char *path, const int16_t *samples, uint32_t n, uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data_bytes = n * 2;
    uint8_t hdr[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    uint32_t riff = 36 + data_bytes, byte_rate = rate * 2;
    memcpy(hdr + 4, &riff, 4); memcpy(hdr + 24, &rate, 4); memcpy(hdr + 28, &byte_rate, 4); memcpy(hdr + 40, &data_bytes, 4);
    fwrite(hdr, 1, 44, f);
    fwrite(samples, 2, n, f);
    fclose(f);
}

static void usage(void) {
    fprintf(stderr,
        "usage: nes-headless --rom FILE [options]\n"
        "  --frames N          run N frames (default 600)\n"
        "  --keys SPEC         input events: 'frame:BTN+BTN;frame:-' (BTN = A B SELECT START UP DOWN LEFT RIGHT,\n"
        "                      2A 2B 2SELECT ... for the second controller, COIN COIN2 SERVICE; '-' = none)\n"
        "  --input FILE        input script: lines of 'frame BTN+BTN'\n"
        "  --dip HEX           Vs. System DIP switches (bit k = switch k+1 on)\n"
        "  --poke SPEC         writes before a frame: 'frame:ADDR=VAL,ADDR=VAL;frame:...' (hex)\n"
        "  --reset F,F,...     press the reset button before these frames run (as Play > Reset game);\n"
        "                      each reset is timed and reported\n"
        "  --palette FILE      write the 512-colour palette the PNGs use as raw RGB (1536 bytes)\n"
        "  --png FILE          write the last frame as PNG\n"
        "  --png-every N       write FILE with the frame number appended every N frames\n"
        "  --widescreen FILE   write the last frame as a 512x240 PNG with the level beside the picture\n"
        "                      (also every N frames with --png-every); checks the centre against --png\n"
        "  --ws-oam-check      every frame, repeat the engine run the shown sprites came from (the source\n"
        "                      of the objects beside the picture) and compare its sprites with the PPU's\n"
        "  --no-ws-sprites     widescreen PNGs without the objects beside the picture\n"
        "  --no-ws-fill        ... and without the page drawn before an area entered part-way\n"
        "  --ws-walk N         the player may walk N pixels beyond the screen's left edge where the band\n"
        "                      beside it shows the level (the frontend's widescreen views; bandwalk.c)\n"
        "  --allstars ROM      the remastered graphics, from this Super Mario All-Stars ROM\n"
        "  --allstars-png FILE write the last frame remastered (with --png-every N: FILE.NNNNN.png every N frames;\n"
        "                      frames it cannot draw are skipped)\n"
        "  --allstars-wide FILE the same with the widescreen canvas (512x240)\n"
        "  --allstars-dump PREFIX at the end, the SNES sandboxes as files (work RAM, VRAM, CGRAM, OAM,\n"
        "                      registers, the parsed columns; PREFIX-screen.* for the screens')\n"
        "  --wav FILE          write audio to a 48 kHz WAV\n"
        "  --allstars-music    with --allstars: All-Stars' music in it, the arcade's sound effects kept\n"
        "  --trace FILE        write a per-instruction trace (interpreter)\n"
        "  --nestest N         nestest mode: start at $C000 and run N instructions\n"
        "  --blargg            poll the blargg test status at $6000 and exit when done\n"
        "  --coverage FILE     write executed-instruction coverage for the recompiler\n"
        "  --recomp            run recompiled code with interpreter fallback\n"
        "  --frame-hash FILE   write a per-frame state hash (for cosimulation diffs)\n"
        "  --sav FILE          load/save battery RAM from FILE\n"
        "  --ram-dump FILE     write RAM + PRG RAM at the end (with --ram-dump-every N: FILE.NNNNN.bin every N frames)\n"
        "  --quiet             no per-run summary\n");
}

int main(int argc, char **argv) {
    const char *rom = NULL, *png = NULL, *wav = NULL, *trace = NULL, *cov = NULL, *hash = NULL, *sav = NULL, *input = NULL, *debug_log = NULL, *ram_dump = NULL, *pal_out = NULL, *wide = NULL;
    const char *state_save = NULL, *state_load = NULL, *as_rom = NULL, *as_png = NULL, *as_wide = NULL, *as_dump = NULL;
    int frames = 600, png_every = 0, ram_dump_every = 0, nestest = 0, blargg = 0, recomp = 0, quiet = 0, trace_start = 0, dip = 0;
    int oam_check = 0, oam_same = 0, oam_differ = 0, oam_none = 0, first_differ = -1;
    double oam_seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) parse_keys(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--png-every") && i + 1 < argc) png_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--widescreen") && i + 1 < argc) wide = argv[++i];
        else if (!strcmp(argv[i], "--ws-oam-check")) oam_check = 1;
        else if (!strcmp(argv[i], "--no-ws-sprites")) widescreen_sprites = false;
        else if (!strcmp(argv[i], "--no-ws-fill")) widescreen_fill = false;
        else if (!strcmp(argv[i], "--ws-walk") && i + 1 < argc) widescreen_walk = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--allstars") && i + 1 < argc) as_rom = argv[++i];
        else if (!strcmp(argv[i], "--allstars-png") && i + 1 < argc) as_png = argv[++i];
        else if (!strcmp(argv[i], "--allstars-wide") && i + 1 < argc) as_wide = argv[++i];
        else if (!strcmp(argv[i], "--allstars-dump") && i + 1 < argc) as_dump = argv[++i];
        else if (!strcmp(argv[i], "--allstars-music")) allstars_music_enabled = true;
        else if (!strcmp(argv[i], "--dip") && i + 1 < argc) dip = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) parse_pokes(argv[++i]);
        else if (!strcmp(argv[i], "--reset") && i + 1 < argc) parse_resets(argv[++i]);
        else if (!strcmp(argv[i], "--palette") && i + 1 < argc) pal_out = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--nestest") && i + 1 < argc) nestest = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blargg")) blargg = 1;
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) cov = argv[++i];
        else if (!strcmp(argv[i], "--recomp")) recomp = 1;
        else if (!strcmp(argv[i], "--frame-hash") && i + 1 < argc) hash = argv[++i];
        else if (!strcmp(argv[i], "--sav") && i + 1 < argc) sav = argv[++i];
        else if (!strcmp(argv[i], "--state-save") && i + 1 < argc) state_save = argv[++i];
        else if (!strcmp(argv[i], "--state-load") && i + 1 < argc) state_load = argv[++i];
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--no-sprite-limit")) ppu_no_sprite_limit = true;
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch_addr = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--trace-start") && i + 1 < argc) trace_start = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--debug-log") && i + 1 < argc) debug_log = argv[++i];
        else if (!strcmp(argv[i], "--ram-dump") && i + 1 < argc) ram_dump = argv[++i];
        else if (!strcmp(argv[i], "--ram-dump-every") && i + 1 < argc) ram_dump_every = atoi(argv[++i]);
        else { usage(); return 2; }
    }
    if (!rom) { usage(); return 2; }

    apu_set_sample_rate(48000);
    sys.vs_dip = (uint8_t)dip;
    if (nes_load(rom) != 0) return 1;
    if (sav) cart_load_battery(sav);
    if (state_load) {
        size_t len = nes_state_size();
        void *buf = malloc(len);
        FILE *f = nes_fopen(state_load, "rb");
        size_t n = f ? fread(buf, 1, len, f) : 0;
        if (f) fclose(f);
        if (n != len || nes_state_load(buf, len) != 0) { fprintf(stderr, "cannot load state %s\n", state_load); return 1; }
        free(buf);
        if (!quiet) printf("state loaded from %s (cpu cycle %llu)\n", state_load, (unsigned long long)cpu.cycles);
    }
    if (cart.vs) nes_palette_build_rgb(pal, cart.vs_ppu);
    else nes_palette_build(pal);
    if (pal_out) {
        FILE *f = fopen(pal_out, "wb");
        for (int i = 0; f && i < 512; i++) { uint8_t c[3] = { (uint8_t)(pal[i] >> 16), (uint8_t)(pal[i] >> 8), (uint8_t)pal[i] }; fwrite(c, 1, 3, f); }
        if (f) fclose(f);
    }
    if (input) load_input_script(input);
    if (as_rom) {
        char err[256];
        if (allstars_load(as_rom, err, sizeof err) != 0) { fprintf(stderr, "All-Stars ROM %s: %s\n", as_rom, err); return 1; }
        if (!quiet) printf("All-Stars ROM: %s (%s)\n", allstars_name(), allstars_sha1());
    }
    if (wide || oam_check || widescreen_walk || as_rom) widescreen_attach();

    if (recomp) {
        if (!recomp_rom_sha1) { fprintf(stderr, "this build has no recompiled code\n"); return 1; }
        if (!recomp_fits_cart()) {
            fprintf(stderr, "recompiled code was generated from ROM %s, which this ROM (%s) is not\n", recomp_rom_sha1, cart.sha1_hex);
            return 1;
        }
        sys.use_recomp = true;
    }
    if (trace) {
        trace_file = !strcmp(trace, "-") ? stdout : fopen(trace, "w");
        if (!trace_file) { fprintf(stderr, "cannot open %s\n", trace); return 1; }
        if (trace_start == 0) sys.trace_hook = trace_hook;
    }
    if (debug_log) sys.debug_log = fopen(debug_log, "w");
    if (cov) {
        sys.cov = calloc(cart.prg_size + 0x8000, 1);
        /* load an existing coverage file so runs accumulate */
        FILE *f = fopen(cov, "rb");
        if (f) { size_t n = fread(sys.cov, 1, cart.prg_size + 0x8000, f); (void)n; fclose(f); }
    }

    if (nestest) {
        cpu.pc = 0xC000;
        for (int i = 0; i < nestest; i++) {
            if (cpu.int_pending) cpu_take_interrupt();
            cpu_step();
        }
        if (trace_file && trace_file != stdout) fclose(trace_file);
        printf("nestest: $02=%02X $03=%02X\n", ram[2], ram[3]);
        return 0;
    }

    FILE *hash_file = hash ? fopen(hash, "w") : NULL;
    int16_t *audio = wav ? malloc(sizeof(int16_t) * 48000u * (size_t)(frames / 60 + 2)) : NULL;
    uint32_t audio_n = 0, audio_cap = wav ? 48000u * (uint32_t)(frames / 60 + 2) : 0;
    uint8_t *rgb = malloc(256 * 240 * 3);
    uint8_t *rgb_wide = malloc(WS_W * WS_H * 3);
    int wide_frames = 0, wide_levels = 0, wide_mismatch = 0;
    long wide_bad = 0, wide_total = 0;
    int ev = 0;
    int blargg_result = -1;
    int reset_hold = 0;

    for (int frame = 0; frame < frames; frame++) {
        if (trace_file && trace_start && frame == trace_start) sys.trace_hook = trace_hook;
        for (int k = 0; k < n_resets; k++) {
            if (reset_frames[k] != frame) continue;
            uint64_t before = ppu.frame;
            double t0 = now_ms();
            nes_reset();
            double ms = now_ms() - t0;
            printf("reset before frame %d: %.3f ms; PPU frame %llu -> %llu, PPU dots %s CPU cycles x 3\n", frame, ms,
                   (unsigned long long)before, (unsigned long long)ppu.frame, ppu.dots == cpu.cycles * 3 ? "=" : "!=");
            fflush(stdout);
        }
        while (ev < n_events && events[ev].frame <= frame) {
            nes_set_pad(0, (uint8_t)events[ev].buttons);
            nes_set_pad(1, (uint8_t)(events[ev].buttons >> 8));
            nes_set_vs_coin((uint8_t)(events[ev].buttons >> 16));
            ev++;
        }
        for (int k = 0; k < n_pokes; k++)
            if (pokes[k].frame == frame) mem_write(pokes[k].addr, pokes[k].val);
        nes_run_frame();
        if (hash_file) fprintf(hash_file, "%d %016llx\n", frame, (unsigned long long)frame_hash());
        if (oam_check) {
            clock_t t0 = clock();
            int r = widescreen_sprite_check();
            oam_seconds += (double)(clock() - t0) / CLOCKS_PER_SEC;
            if (r > 0) oam_same++;
            else if (r == 0) { oam_differ++; if (first_differ < 0) first_differ = frame; }
            else oam_none++;
        }
        if (allstars_music_enabled) allstars_music_frame();
        if (audio) {
            uint32_t avail = apu_samples_available();
            if (audio_n + avail > audio_cap) avail = audio_cap - audio_n;
            uint32_t got = apu_read_samples(audio + audio_n, avail);
            allstars_music_mix(audio + audio_n, got, 48000);
            audio_n += got;
        } else {
            apu.ring_read = apu.ring_write;
        }
        if (ram_dump && ram_dump_every && (frame % ram_dump_every) == 0) {
            char name[1100];
            snprintf(name, sizeof name, "%s.%05d.bin", ram_dump, frame);
            FILE *f = fopen(name, "wb");
            if (f) {   /* RAM, work RAM, then the PPU's nametable RAM and palette */
                fwrite(ram, 1, sizeof ram, f); fwrite(cart.wram, 1, sizeof cart.wram, f);
                fwrite(ppu.vram, 1, sizeof ppu.vram, f); fwrite(ppu.palette, 1, sizeof ppu.palette, f);
                fclose(f);
            }
        }
        if (png && png_every && (frame % png_every) == 0) {
            char name[1100];
            snprintf(name, sizeof name, "%s.%05d.png", png, frame);
            fb_to_rgb(rgb);
            png_write_rgb(name, 256, 240, rgb);
        }
        if ((as_png || as_wide) && png_every && (frame % png_every) == 0) {
            char name[1100];
            if (as_png && allstars_to_rgb(rgb, 0)) {
                snprintf(name, sizeof name, "%s.%05d.png", as_png, frame);
                png_write_rgb(name, 256, 240, rgb);
            }
            if (as_wide && allstars_to_rgb(rgb_wide, 1)) {
                snprintf(name, sizeof name, "%s.%05d.png", as_wide, frame);
                png_write_rgb(name, WS_W, WS_H, rgb_wide);
            }
        }
        if (wide && png_every && (frame % png_every) == 0) {
            char name[1100];
            snprintf(name, sizeof name, "%s.%05d.png", wide, frame);
            wide_levels += wide_to_rgb(rgb_wide); wide_frames++;
            png_write_rgb(name, WS_W, WS_H, rgb_wide);
            int bp, tp;
            if (widescreen_check(&bp, &tp) == 0) { wide_bad += bp; wide_total += tp; }
        }
        if (blargg && reset_hold > 0) { reset_hold--; continue; }
        if (blargg && cart.wram[1] == 0xDE && cart.wram[2] == 0xB0 && cart.wram[3] == 0x61) {
            uint8_t st = cart.wram[0];
            if (st == 0x81) {
                /* the test asks for a reset after ~100 ms; give it a few frames, then ignore status briefly */
                for (int k = 0; k < 8; k++) nes_run_frame();
                nes_reset();
                reset_hold = 10;
                continue;
            }
            if (st != 0x80) {
                blargg_result = st;
                printf("blargg: status %02X\n%s\n", st, (const char *)cart.wram + 4);
                break;
            }
        }
    }

    if (png) { fb_to_rgb(rgb); png_write_rgb(png, 256, 240, rgb); }
    if (as_png && !png_every) { if (allstars_to_rgb(rgb, 0)) png_write_rgb(as_png, 256, 240, rgb); else printf("allstars: the last frame was not drawn remastered\n"); }
    if (as_wide && !png_every) { if (allstars_to_rgb(rgb_wide, 1)) png_write_rgb(as_wide, WS_W, WS_H, rgb_wide); }
    if (as_dump) allstars_debug_dump(as_dump);
    if (wide) {
        /* the centre of the canvas must be the normal frame, pixel for pixel, except in the 8-pixel columns
           at its edges, where the sprite columns the game hides are drawn back */
        int level = wide_to_rgb(rgb_wide), edge_pixels = 0;
        wide_levels += level; wide_frames++;
        fb_to_rgb(rgb);
        for (int y = 0; y < 240; y++) {
            const uint8_t *a = rgb + y * 256 * 3, *b = rgb_wide + ((y + WS_VBAND) * WS_W + WS_BAND) * 3;
            if (memcmp(a + 8 * 3, b + 8 * 3, 240 * 3) != 0) wide_mismatch++;
            for (int x = 0; x < 256; x += (x == 7 ? 241 : 1))
                if (memcmp(a + x * 3, b + x * 3, 3) != 0) edge_pixels++;
        }
        png_write_rgb(wide, WS_W, WS_H, rgb_wide);
        int bp, tp;
        if (widescreen_check(&bp, &tp) == 0) { wide_bad += bp; wide_total += tp; }
        printf("widescreen: %s, bands %s, centre %s the normal frame (%d of %d frames showed a level); %d pixels of hidden sprite columns drawn at its edges\n", wide,
               level ? "show the level" : "black (no level on screen)", wide_mismatch ? "DIFFERS from" : "identical to",
               wide_levels, wide_frames, edge_pixels);
        printf("widescreen: the centre re-rendered from the nametables differs from the PPU's in %.2f%% of the pixels (sprites)\n",
               wide_total ? 100.0 * wide_bad / wide_total : 0.0);
    }
    if (oam_check) {
        int runs = oam_same + oam_differ;
        char first[48] = "";
        if (oam_differ) snprintf(first, sizeof first, " (the first at frame %d)", first_differ);
        printf("ws-oam-check: %d engine runs repeated with the PPU's sprites exactly, %d with different ones%s, "
               "%d frames without a run to repeat; %.3f ms per repeat\n",
               oam_same, oam_differ, first, oam_none, runs ? 1000.0 * oam_seconds / runs : 0.0);
    }
    if (wav) { write_wav(wav, audio, audio_n, 48000); free(audio); }
    if (hash_file) fclose(hash_file);
    if (trace_file && trace_file != stdout) fclose(trace_file);
    if (cov) {
        FILE *f = fopen(cov, "wb");
        if (f) { fwrite(sys.cov, 1, cart.prg_size + 0x8000, f); fclose(f); }
    }
    if (sav) cart_save_battery(sav);
    if (state_save) {
        size_t len = nes_state_size();
        void *buf = malloc(len);
        FILE *f = nes_fopen(state_save, "wb");
        if (!f || nes_state_save(buf, len) != 0) { fprintf(stderr, "cannot save state %s\n", state_save); return 1; }
        fwrite(buf, 1, len, f);
        fclose(f);
        free(buf);
        if (!quiet) printf("state saved to %s (%zu bytes, cpu cycle %llu)\n", state_save, len, (unsigned long long)cpu.cycles);
    }
    if (sys.debug_log) fclose((FILE *)sys.debug_log);
    if (ram_dump) {
        FILE *f = fopen(ram_dump, "wb");
        if (f) { fwrite(ram, 1, sizeof ram, f); fwrite(cart.wram, 1, sizeof cart.wram, f); fclose(f); }
    }
    if (!quiet) {
        printf("ran %d frames, %llu instructions, cpu cycles %llu, ppu frame %llu\n", frames,
               (unsigned long long)sys.instructions, (unsigned long long)cpu.cycles, (unsigned long long)ppu.frame);
        if (cart.vs)
            printf("ppu ctrl %02X mask %02X | vs: latch %02X (CHR bank %d), DIP %02X, coin meter %u, irqs %llu\n",
                   ppu.ctrl, ppu.mask, mapper.vs_latch, (mapper.vs_latch >> 2) & 1, sys.vs_dip,
                   sys.vs_coin_meter & 0x7FFFFFFFu, (unsigned long long)sys.irq_count);
        else
            printf("ppu ctrl %02X mask %02X | mapper: bank_select %02X regs %02X %02X %02X %02X %02X %02X %02X %02X mirror %02X wram %02X | irq latch %d counter %d enabled %d | a12 clocks %llu, irqs %llu\n",
                   ppu.ctrl, ppu.mask, mapper.bank_select, mapper.regs[0], mapper.regs[1], mapper.regs[2], mapper.regs[3],
                   mapper.regs[4], mapper.regs[5], mapper.regs[6], mapper.regs[7], mapper.mirroring, mapper.wram_ctrl,
                   mapper.irq_latch, mapper.irq_counter, mapper.irq_enabled, (unsigned long long)sys.a12_clocks, (unsigned long long)sys.irq_count);
        if (sys.use_recomp) printf("recomp: %llu native routine entries, %llu interpreter fallback instructions\n",
                                   (unsigned long long)sys.native_blocks, (unsigned long long)sys.fallback_steps);
    }
    free(rgb);
    free(rgb_wide);
    if (wide && wide_mismatch) return 1;
    if (oam_check && oam_differ) return 1;
    if (blargg) return blargg_result == 0 ? 0 : 1;
    return 0;
}
