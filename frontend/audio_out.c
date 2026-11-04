/*
 * audio_out.c - queues the APU's samples to an SDL audio device.
 *
 * Latency is kept low and steady with dynamic rate control: the APU's output
 * sample rate is nudged by up to +-1.2% so the queue hovers around a target
 * instead of drifting to a large buffer (which the player feels as input lag,
 * since every sound effect arrives late) or running dry.
 */
#include "audio_out.h"
#include <SDL.h>
#include "nes.h"

static const struct { int target_ms, max_ms, prime_ms, device_samples; } presets[3] = {
    { 16, 60, 10, 256 },     /* low: ~25 ms including the device buffer */
    { 32, 100, 20, 512 },    /* normal */
    { 70, 180, 50, 1024 },   /* safe: for systems that stutter */
};

static SDL_AudioDeviceID dev;
static int base_rate, cur_rate, preset;
static double trim;          /* learned rate correction (integral term) */

static int open_device(void) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = base_rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = (Uint16)presets[preset].device_samples;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) return 0;
    audio_out_clear();
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}

int audio_out_init(int sample_rate, int p) {
    base_rate = cur_rate = sample_rate;
    preset = (p < 0) ? 0 : (p > 2 ? 2 : p);
    return open_device();
}

void audio_out_set_preset(int p) {
    p = (p < 0) ? 0 : (p > 2 ? 2 : p);
    if (p == preset && dev) return;
    preset = p;
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
    if (base_rate) open_device();
}

void audio_out_clear(void) {
    if (!dev) return;
    static int16_t silence[9600];
    SDL_ClearQueuedAudio(dev);
    Uint32 n = (Uint32)(base_rate * presets[preset].prime_ms / 1000);
    if (n > 9600) n = 9600;
    SDL_QueueAudio(dev, silence, n * sizeof(int16_t));
}

int audio_out_queued_ms(void) {
    if (!dev || !base_rate) return 0;
    return (int)((SDL_GetQueuedAudioSize(dev) / sizeof(int16_t)) * 1000u / (unsigned)base_rate);
}

int audio_out_rate(void) { return cur_rate; }

void audio_out_pump(int muted, int volume) {
    static int16_t buf[8192];
    if (dev && !muted) {
        int target = presets[preset].target_ms;
        double err = (double)(target - audio_out_queued_ms()) / target;   /* +1 empty .. -1 at twice the target */
        if (err > 1.0) err = 1.0;
        if (err < -1.0) err = -1.0;
        /* a slow integral term learns a steady clock difference (a 120.6 Hz display paces the
           game 0.35 % fast), so the queue settles at the target instead of above it */
        trim += 0.000024 * err;
        if (trim > 0.01) trim = 0.01;
        if (trim < -0.01) trim = -0.01;
        cur_rate = (int)(base_rate * (1.0 + 0.012 * err + trim) + 0.5);
    } else {
        cur_rate = base_rate;
    }
    apu.sample_rate = (uint32_t)cur_rate;

    uint32_t n = apu_samples_available();
    while (n > 0) {
        uint32_t got = apu_read_samples(buf, n < 8192 ? n : 8192);
        if (!got) break;
        n -= got;
        if (muted || !dev) continue;
        if (SDL_GetQueuedAudioSize(dev) > (Uint32)(base_rate * presets[preset].max_ms / 1000) * sizeof(int16_t)) continue;
        if (volume != 100) for (uint32_t i = 0; i < got; i++) buf[i] = (int16_t)(buf[i] * volume / 100);
        SDL_QueueAudio(dev, buf, got * sizeof(int16_t));
    }
}

void audio_out_shutdown(void) {
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}
