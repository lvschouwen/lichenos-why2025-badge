#pragma once
// Notification sounds (#67). Pure: a sound is a short list of (frequency, duration, gap) steps, so what the speaker
// does is a function of the time since it started. Samples are generated on the fly from a phase accumulator, so
// there is no PCM table. The host test checks the shapes, the volume curve, the gating, the rate limit and the
// generator.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AU_INFO, AU_MESSAGE, AU_ALERT, AU_COUNT } au_level_t;

// Two sounds a notice makes at most this often (a burst of notices gives one sound, not a queue)
#define AU_NOTIFY_GAP_MS 2000
// Volume setting steps: 0 is off, 10 is loudest
#define AU_VOLUME_MAX 10

typedef struct {
    uint16_t freq_hz; // 0 = rest
    uint16_t dur_ms;
    uint16_t gap_ms;  // silence after the tone
} au_step_t;

// The steps of a level (an unknown level gives AU_INFO's); count out
au_step_t const *au_steps(au_level_t l, size_t *count);
// Length of the sound: the end of the last tone (the trailing gap is not waited for)
uint32_t au_total_ms(au_level_t l);
// Frequency t_ms after the start: 0 in a gap or when done
uint16_t au_freq_at(au_level_t l, uint32_t t_ms);
bool     au_done(au_level_t l, uint32_t t_ms);

// notify level (0 info, 1 warn, 2 error) -> sound level
au_level_t au_for_notice(uint8_t level);

// Volume step (0..AU_VOLUME_MAX, larger is clamped) -> peak sample amplitude. Quadratic, since loudness is
// perceived roughly logarithmically; the top is below full scale so a square wave does not clip the amp.
int16_t au_amplitude(uint8_t volume);

// A sound is allowed: the setting is on, do-not-disturb is off and the volume is not 0
bool au_allowed(bool enabled, bool dnd, uint8_t volume);

typedef struct {
    uint32_t last_ms;
    bool     any;
} au_gate_t;
// True when now_ms is at least min_gap_ms after the last accepted one (wrap-safe); an accepted one is recorded
bool au_gate_take(au_gate_t *g, uint32_t now_ms, uint32_t min_gap_ms);

typedef enum { AU_SQUARE, AU_SINE } au_wave_t;

// Fills n mono samples of a tone of freq_hz at sample_rate. *phase is a 32-bit phase accumulator the caller keeps
// between buffers so the wave stays continuous (start it at 0). freq_hz 0, volume 0 or sample_rate 0 gives
// silence. Frequencies at or above half the sample rate are clamped just under it.
void au_fill(int16_t *buf, size_t n, uint32_t sample_rate, uint16_t freq_hz, uint8_t volume, au_wave_t wave,
             uint32_t *phase);

#ifdef __cplusplus
}
#endif
