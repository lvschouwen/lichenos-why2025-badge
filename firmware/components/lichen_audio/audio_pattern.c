#include "audio_pattern.h"

#include <string.h>

// Short enough for the speaker to be a hint, not a siren: one high blip, a rising pair, a low-high alarm
static au_step_t const INFO[]    = {{1568, 60, 0}};
static au_step_t const MESSAGE[] = {{1319, 70, 40}, {1760, 90, 0}};
static au_step_t const ALERT[]   = {{880, 150, 60}, {1760, 150, 60}, {880, 150, 60}, {1760, 150, 0}};

au_step_t const *au_steps(au_level_t l, size_t *count) {
    switch (l) {
    case AU_MESSAGE:
        *count = sizeof(MESSAGE) / sizeof(MESSAGE[0]);
        return MESSAGE;
    case AU_ALERT:
        *count = sizeof(ALERT) / sizeof(ALERT[0]);
        return ALERT;
    case AU_INFO:
    default:
        *count = sizeof(INFO) / sizeof(INFO[0]);
        return INFO;
    }
}

uint32_t au_total_ms(au_level_t l) {
    size_t           n;
    au_step_t const *st    = au_steps(l, &n);
    uint32_t         total = 0;
    for (size_t i = 0; i < n; i++) {
        total += st[i].dur_ms + (i + 1 < n ? st[i].gap_ms : 0);
    }
    return total;
}

uint16_t au_freq_at(au_level_t l, uint32_t t_ms) {
    size_t           n;
    au_step_t const *st = au_steps(l, &n);
    for (size_t i = 0; i < n; i++) {
        if (t_ms < st[i].dur_ms) {
            return st[i].freq_hz;
        }
        t_ms -= st[i].dur_ms;
        if (t_ms < st[i].gap_ms) {
            return 0;
        }
        t_ms -= st[i].gap_ms;
    }
    return 0;
}

bool au_done(au_level_t l, uint32_t t_ms) {
    return t_ms >= au_total_ms(l);
}

au_level_t au_for_notice(uint8_t level) {
    return level >= 2 ? AU_ALERT : level == 1 ? AU_MESSAGE : AU_INFO;
}

int16_t au_amplitude(uint8_t volume) {
    uint32_t v = volume > AU_VOLUME_MAX ? AU_VOLUME_MAX : volume;
    return (int16_t)(24000u * v * v / (AU_VOLUME_MAX * AU_VOLUME_MAX));
}

bool au_allowed(bool enabled, bool dnd, uint8_t volume) {
    return enabled && !dnd && volume > 0;
}

bool au_gate_take(au_gate_t *g, uint32_t now_ms, uint32_t min_gap_ms) {
    if (g->any && (uint32_t)(now_ms - g->last_ms) < min_gap_ms) {
        return false;
    }
    g->any     = true;
    g->last_ms = now_ms;
    return true;
}

// Phase 0..2^32 is one period; result is -32768..32768 (Q15). Sine is a parabola per half period (error about 1 %,
// plenty for a beep and no table): with x in 0..1 over the half, 4x(1-x) rises 0 -> 1 -> 0.
static int32_t wave_unit(au_wave_t wave, uint32_t phase) {
    bool first_half = phase < 0x80000000u;
    if (wave == AU_SQUARE) {
        return first_half ? 32768 : -32768;
    }
    uint32_t x    = (phase << 1) >> 17;                       // position in the half period, 0..32767
    int32_t  para = (int32_t)((4u * x * (32768u - x)) >> 15); // 0..32768
    return first_half ? para : -para;
}

void au_fill(int16_t *buf, size_t n, uint32_t sample_rate, uint16_t freq_hz, uint8_t volume, au_wave_t wave,
             uint32_t *phase) {
    int32_t amp = au_amplitude(volume);
    if (sample_rate == 0 || freq_hz == 0 || amp == 0) {
        memset(buf, 0, n * sizeof(buf[0]));
        return;
    }
    uint64_t f = freq_hz;
    if (f * 2 >= sample_rate) {
        f = (sample_rate - 1) / 2;
    }
    uint32_t step = (uint32_t)((f << 32) / sample_rate);
    uint32_t ph   = *phase;
    for (size_t i = 0; i < n; i++) {
        buf[i] = (int16_t)((wave_unit(wave, ph) * amp) >> 15);
        ph += step;
    }
    *phase = ph;
}
