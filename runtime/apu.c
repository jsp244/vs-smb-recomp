/*
 * apu.c - 2A03 APU: two pulses, triangle, noise, DMC, frame counter,
 * non-linear mixer, resampling to the host rate with the NES's
 * analogue output filters.
 */
#include "nes.h"
#include <math.h>

apu_t apu;
static int16_t apu_ring[APU_RING_SIZE];   /* output samples; indices in apu_t */

void apu_flush_samples(void) { apu.ring_read = apu.ring_write; }

/* not machine state: the channels heard (bit 0 square 1, 1 square 2, 2 triangle, 3 noise, 4 DMC),
   asked of the hook after each catch-up for the stretch that follows. While some are left out,
   that mix goes through its own copy of the output filters to the ring, and the machine's state
   (the averaging and the filters in apu_t) still follows the whole mix, so it is the same either way */
uint8_t (*apu_mask_hook)(void);
static uint8_t apu_span_mask = 0x1F;
static struct { int on; double acc, hp90_in, hp90_out, hp440_in, hp440_out, lp; } masked;

#define CPU_HZ 1789773u

static const uint8_t length_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};
static const uint16_t noise_period[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t dmc_rate[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
static const uint8_t duty_seq[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 1, 0, 0, 0, 0, 0 }, { 0, 1, 1, 1, 1, 0, 0, 0 }, { 1, 0, 0, 1, 1, 1, 1, 1 }
};
static const uint8_t tri_seq[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

static float pulse_table[31], tnd_table[203];
static double hp90_a, hp440_a, lp_b;

static void build_tables(void) {
    pulse_table[0] = 0;
    for (int i = 1; i < 31; i++) pulse_table[i] = (float)(95.52 / (8128.0 / i + 100.0));
    tnd_table[0] = 0;
    for (int i = 1; i < 203; i++) tnd_table[i] = (float)(163.67 / (24329.0 / i + 100.0));
}

void apu_set_sample_rate(uint32_t hz) {
    apu.sample_rate = hz;
    double dt = 1.0 / hz;
    double rc90 = 1.0 / (2 * M_PI * 90.0), rc440 = 1.0 / (2 * M_PI * 440.0), rc14k = 1.0 / (2 * M_PI * 14000.0);
    hp90_a = rc90 / (rc90 + dt);
    hp440_a = rc440 / (rc440 + dt);
    lp_b = dt / (rc14k + dt);
}

void apu_reset(void) {
    uint32_t rate = apu.sample_rate ? apu.sample_rate : 48000;
    memset(&apu, 0, sizeof apu);
    build_tables();
    apu_set_sample_rate(rate);
    apu.noise.shift = 1;
    apu.dmc.sample_buffer_empty = true;
    apu.dmc.silence = true;
    apu.dmc.bits_remaining = 8;
    apu.dmc.timer_period = dmc_rate[0];
    apu.noise.timer_period = noise_period[0];
    apu.frame_counter = 0;
    apu.mode5 = false;
    apu.irq_inhibit = false;
}

/* ---- envelopes / counters ---- */
static inline void envelope_clock(bool *start, uint8_t *divider, uint8_t *decay, uint8_t period, bool loop) {
    if (*start) { *start = false; *decay = 15; *divider = period; return; }
    if (*divider == 0) {
        *divider = period;
        if (*decay > 0) (*decay)--;
        else if (loop) *decay = 15;
    } else (*divider)--;
}

static inline uint16_t sweep_target(apu_pulse_t *p, int idx) {
    uint16_t delta = p->timer_period >> p->sweep_shift;
    if (p->sweep_negate) return (uint16_t)(p->timer_period - delta - (idx == 0 ? 1 : 0));
    return (uint16_t)(p->timer_period + delta);
}

static inline bool pulse_muted(apu_pulse_t *p, int idx) {
    return p->timer_period < 8 || sweep_target(p, idx) > 0x7FF;
}

static void quarter_frame(void) {
    for (int i = 0; i < 2; i++) {
        apu_pulse_t *p = &apu.pulse[i];
        envelope_clock(&p->env_start, &p->env_divider, &p->env_decay, p->volume, p->length_halt);
    }
    envelope_clock(&apu.noise.env_start, &apu.noise.env_divider, &apu.noise.env_decay, apu.noise.volume, apu.noise.length_halt);
    apu_tri_t *t = &apu.tri;
    if (t->linear_reload) t->linear = t->linear_load;
    else if (t->linear > 0) t->linear--;
    if (!t->control) t->linear_reload = false;
}

static void half_frame(void) {
    for (int i = 0; i < 2; i++) {
        apu_pulse_t *p = &apu.pulse[i];
        if (p->length && !p->length_halt) p->length--;
        if (p->sweep_divider == 0 && p->sweep_enabled && p->sweep_shift && !pulse_muted(p, i))
            p->timer_period = sweep_target(p, i);
        if (p->sweep_divider == 0 || p->sweep_reload) { p->sweep_divider = p->sweep_period; p->sweep_reload = false; }
        else p->sweep_divider--;
    }
    if (apu.tri.length && !apu.tri.control) apu.tri.length--;
    if (apu.noise.length && !apu.noise.length_halt) apu.noise.length--;
}

static inline void set_frame_irq(void) {
    if (!apu.irq_inhibit) { apu.frame_irq = true; cpu_raise_irq(IRQ_APU_FRAME, apu.cycles); }
}

static void frame_counter_step(void) {
    uint32_t c = ++apu.frame_counter;
    if (!apu.mode5) {
        switch (c) {
        case 7457: quarter_frame(); break;
        case 14913: quarter_frame(); half_frame(); break;
        case 22371: quarter_frame(); break;
        case 29828: set_frame_irq(); break;
        case 29829: quarter_frame(); half_frame(); set_frame_irq(); break;
        case 29830: set_frame_irq(); apu.frame_counter = 0; break;
        default: break;
        }
    } else {
        switch (c) {
        case 7457: quarter_frame(); break;
        case 14913: quarter_frame(); half_frame(); break;
        case 22371: quarter_frame(); break;
        case 37281: quarter_frame(); half_frame(); break;
        case 37282: apu.frame_counter = 0; break;
        default: break;
        }
    }
}

/* ---- DMC ---- */
static void dmc_restart(void) {
    apu.dmc.cur_addr = apu.dmc.sample_addr;
    apu.dmc.bytes_remaining = apu.dmc.sample_len;
}

static void dmc_fill_buffer(void) {
    apu_dmc_t *d = &apu.dmc;
    if (!d->sample_buffer_empty || d->bytes_remaining == 0) return;
    d->sample_buffer = mem_read(d->cur_addr);
    d->sample_buffer_empty = false;
    d->cur_addr = (d->cur_addr == 0xFFFF) ? 0x8000 : (uint16_t)(d->cur_addr + 1);
    if (--d->bytes_remaining == 0) {
        if (d->loop) dmc_restart();
        else if (d->irq_enable) { d->irq_flag = true; cpu_raise_irq(IRQ_APU_DMC, apu.cycles); }
    }
}

static inline void dmc_clock(void) {
    apu_dmc_t *d = &apu.dmc;
    if (!d->silence) {
        if (d->shift & 1) { if (d->output <= 125) d->output += 2; }
        else { if (d->output >= 2) d->output -= 2; }
    }
    d->shift >>= 1;
    if (--d->bits_remaining == 0) {
        d->bits_remaining = 8;
        if (d->sample_buffer_empty) d->silence = true;
        else {
            d->silence = false;
            d->shift = d->sample_buffer;
            d->sample_buffer_empty = true;
            dmc_fill_buffer();
        }
    }
}

/* ---- output ---- */
static inline void emit_sample(float v, float vm) {
    double x = v;
    double y1 = x - apu.hp90_prev_in + hp90_a * apu.hp90_prev_out;
    apu.hp90_prev_in = x; apu.hp90_prev_out = y1;
    double y2 = y1 - apu.hp440_prev_in + hp440_a * apu.hp440_prev_out;
    apu.hp440_prev_in = y1; apu.hp440_prev_out = y2;
    apu.lp_prev += lp_b * (y2 - apu.lp_prev);
    double s = apu.lp_prev * 32767.0 * 1.4;
    if (masked.on) {
        double m = vm;
        double z1 = m - masked.hp90_in + hp90_a * masked.hp90_out;
        masked.hp90_in = m; masked.hp90_out = z1;
        double z2 = z1 - masked.hp440_in + hp440_a * masked.hp440_out;
        masked.hp440_in = z1; masked.hp440_out = z2;
        masked.lp += lp_b * (z2 - masked.lp);
        s = masked.lp * 32767.0 * 1.4;
    }
    if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
    uint32_t next = (apu.ring_write + 1) & (APU_RING_SIZE - 1);
    if (next != apu.ring_read) { apu_ring[apu.ring_write] = (int16_t)s; apu.ring_write = next; }
    apu.samples_produced++;
}

static inline void apu_cycle(void) {
    /* delayed $4017 effect: the sequencer restarts from 0 on this cycle */
    if (apu.frame_reset_delay > 0 && --apu.frame_reset_delay == 0) {
        apu.frame_counter = 0;
        apu.mode5 = (apu.frame_reset_value & 0x80) != 0;
        if (apu.mode5) { quarter_frame(); half_frame(); }
    } else {
        frame_counter_step();
    }

    /* pulse timers run at half the CPU rate */
    apu.odd_cycle = !apu.odd_cycle;
    if (apu.odd_cycle) {
        for (int i = 0; i < 2; i++) {
            apu_pulse_t *p = &apu.pulse[i];
            if (p->timer == 0) { p->timer = p->timer_period; p->seq_pos = (uint8_t)((p->seq_pos - 1) & 7); }
            else p->timer--;
        }
    }
    /* triangle */
    {
        apu_tri_t *t = &apu.tri;
        if (t->timer == 0) {
            t->timer = t->timer_period;
            if (t->length && t->linear) t->seq_pos = (uint8_t)((t->seq_pos + 1) & 31);
        } else t->timer--;
    }
    /* noise */
    {
        apu_noise_t *n = &apu.noise;
        if (n->timer == 0) {
            n->timer = (uint16_t)(n->timer_period - 1);
            uint16_t fb = (uint16_t)((n->shift & 1) ^ ((n->shift >> (n->mode ? 6 : 1)) & 1));
            n->shift = (uint16_t)((n->shift >> 1) | (fb << 14));
        } else n->timer--;
    }
    /* dmc */
    {
        apu_dmc_t *d = &apu.dmc;
        if (d->timer == 0) { d->timer = (uint16_t)(d->timer_period - 1); dmc_clock(); }
        else d->timer--;
    }

    /* mix */
    int p1 = 0, p2 = 0;
    for (int i = 0; i < 2; i++) {
        apu_pulse_t *p = &apu.pulse[i];
        int out = 0;
        if (p->length && !pulse_muted(p, i) && duty_seq[p->duty][p->seq_pos])
            out = p->const_volume ? p->volume : p->env_decay;
        if (i == 0) p1 = out; else p2 = out;
    }
    int tri = (apu.tri.timer_period < 2) ? 7 : tri_seq[apu.tri.seq_pos];
    int noise = 0;
    if (apu.noise.length && !(apu.noise.shift & 1))
        noise = apu.noise.const_volume ? apu.noise.volume : apu.noise.env_decay;
    float out = pulse_table[p1 + p2] + tnd_table[3 * tri + 2 * noise + apu.dmc.output];
    if (apu_span_mask != 0x1F) {
        if (!masked.on) {   /* from the whole mix's filters, so it starts without a step */
            masked.on = 1;
            masked.acc = apu.acc;
            masked.hp90_in = apu.hp90_prev_in; masked.hp90_out = apu.hp90_prev_out;
            masked.hp440_in = apu.hp440_prev_in; masked.hp440_out = apu.hp440_prev_out;
            masked.lp = apu.lp_prev;
        }
        int m1 = (apu_span_mask & 1) ? p1 : 0, m2 = (apu_span_mask & 2) ? p2 : 0;
        int mt = (apu_span_mask & 4) ? tri : 0, mn = (apu_span_mask & 8) ? noise : 0, md = (apu_span_mask & 16) ? apu.dmc.output : 0;
        masked.acc += pulse_table[m1 + m2] + tnd_table[3 * mt + 2 * mn + md];
    } else masked.on = 0;
    apu.acc += out;
    apu.acc_n++;
    apu.sample_phase += apu.sample_rate;
    if (apu.sample_phase >= CPU_HZ) {
        apu.sample_phase -= CPU_HZ;
        emit_sample((float)(apu.acc / apu.acc_n), (float)(masked.acc / apu.acc_n));
        apu.acc = 0; apu.acc_n = 0; masked.acc = 0;
    }
}

void apu_run_to(uint64_t target) {
    while (apu.cycles < target) {
        apu_cycle();
        apu.cycles++;
    }
    /* for the stretch up to the next register write: what the game has set by now */
    apu_span_mask = apu_mask_hook ? apu_mask_hook() : 0x1F;
}

uint8_t apu_read_status(void) {
    apu_run_to(cpu.cycles);
    uint8_t r = 0;
    if (apu.pulse[0].length) r |= 0x01;
    if (apu.pulse[1].length) r |= 0x02;
    if (apu.tri.length) r |= 0x04;
    if (apu.noise.length) r |= 0x08;
    if (apu.dmc.bytes_remaining) r |= 0x10;
    if (apu.frame_irq) r |= 0x40;
    if (apu.dmc.irq_flag) r |= 0x80;
    apu.frame_irq = false;
    cpu.irq_line &= (uint8_t)~IRQ_APU_FRAME;
    return r;
}

void apu_write(uint16_t a, uint8_t v) {
    apu_run_to(cpu.cycles);
    switch (a) {
    case 0x4000: case 0x4004: {
        apu_pulse_t *p = &apu.pulse[(a >> 2) & 1];
        p->duty = v >> 6; p->length_halt = (v & 0x20) != 0; p->const_volume = (v & 0x10) != 0; p->volume = v & 0x0F;
        break; }
    case 0x4001: case 0x4005: {
        apu_pulse_t *p = &apu.pulse[(a >> 2) & 1];
        p->sweep_enabled = (v & 0x80) != 0; p->sweep_period = (v >> 4) & 7;
        p->sweep_negate = (v & 0x08) != 0; p->sweep_shift = v & 7; p->sweep_reload = true;
        break; }
    case 0x4002: case 0x4006: {
        apu_pulse_t *p = &apu.pulse[(a >> 2) & 1];
        p->timer_period = (uint16_t)((p->timer_period & 0x700) | v);
        break; }
    case 0x4003: case 0x4007: {
        apu_pulse_t *p = &apu.pulse[(a >> 2) & 1];
        p->timer_period = (uint16_t)((p->timer_period & 0xFF) | ((v & 7) << 8));
        if (p->enabled) p->length = length_table[v >> 3];
        p->seq_pos = 0; p->env_start = true;
        break; }
    case 0x4008:
        apu.tri.control = (v & 0x80) != 0; apu.tri.linear_load = v & 0x7F;
        break;
    case 0x400A:
        apu.tri.timer_period = (uint16_t)((apu.tri.timer_period & 0x700) | v);
        break;
    case 0x400B:
        apu.tri.timer_period = (uint16_t)((apu.tri.timer_period & 0xFF) | ((v & 7) << 8));
        if (apu.tri.enabled) apu.tri.length = length_table[v >> 3];
        apu.tri.linear_reload = true;
        break;
    case 0x400C:
        apu.noise.length_halt = (v & 0x20) != 0; apu.noise.const_volume = (v & 0x10) != 0; apu.noise.volume = v & 0x0F;
        break;
    case 0x400E:
        apu.noise.mode = (v & 0x80) != 0; apu.noise.timer_period = noise_period[v & 0x0F];
        break;
    case 0x400F:
        if (apu.noise.enabled) apu.noise.length = length_table[v >> 3];
        apu.noise.env_start = true;
        break;
    case 0x4010:
        apu.dmc.irq_enable = (v & 0x80) != 0; apu.dmc.loop = (v & 0x40) != 0;
        apu.dmc.rate_index = v & 0x0F; apu.dmc.timer_period = dmc_rate[v & 0x0F];
        if (!apu.dmc.irq_enable) { apu.dmc.irq_flag = false; cpu.irq_line &= (uint8_t)~IRQ_APU_DMC; }
        break;
    case 0x4011: apu.dmc.output = v & 0x7F; break;
    case 0x4012: apu.dmc.sample_addr = (uint16_t)(0xC000 + v * 64); break;
    case 0x4013: apu.dmc.sample_len = (uint16_t)(v * 16 + 1); break;
    case 0x4015:
        apu.pulse[0].enabled = (v & 1) != 0; if (!apu.pulse[0].enabled) apu.pulse[0].length = 0;
        apu.pulse[1].enabled = (v & 2) != 0; if (!apu.pulse[1].enabled) apu.pulse[1].length = 0;
        apu.tri.enabled = (v & 4) != 0; if (!apu.tri.enabled) apu.tri.length = 0;
        apu.noise.enabled = (v & 8) != 0; if (!apu.noise.enabled) apu.noise.length = 0;
        apu.dmc.enabled = (v & 0x10) != 0;
        if (apu.dmc.enabled) { if (apu.dmc.bytes_remaining == 0) { dmc_restart(); dmc_fill_buffer(); } }
        else apu.dmc.bytes_remaining = 0;
        apu.dmc.irq_flag = false; cpu.irq_line &= (uint8_t)~IRQ_APU_DMC;
        break;
    case 0x4017:
        apu.frame_reset_value = v;
        /* write cycle is cpu.cycles-1; even write cycles coincide with an APU tick (3 cycle delay) */
        apu.frame_reset_delay = (cpu.cycles & 1) ? 3 : 4;
        apu.irq_inhibit = (v & 0x40) != 0;
        if (apu.irq_inhibit) { apu.frame_irq = false; cpu.irq_line &= (uint8_t)~IRQ_APU_FRAME; }
        break;
    default: break;
    }
}

uint32_t apu_samples_available(void) {
    return (apu.ring_write - apu.ring_read) & (APU_RING_SIZE - 1);
}

uint32_t apu_read_samples(int16_t *out, uint32_t max) {
    uint32_t n = 0;
    while (n < max && apu.ring_read != apu.ring_write) {
        out[n++] = apu_ring[apu.ring_read];
        apu.ring_read = (apu.ring_read + 1) & (APU_RING_SIZE - 1);
    }
    return n;
}
