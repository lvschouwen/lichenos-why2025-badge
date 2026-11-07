// Host test: notification sound patterns, volume, gating, rate limit and the sample generator (#67)
#include "audio_pattern.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_patterns(void) {
    size_t n;
    au_steps(AU_INFO, &n);
    CHECK(n == 1 && au_total_ms(AU_INFO) == 60);
    au_steps(AU_MESSAGE, &n);
    CHECK(n == 2 && au_total_ms(AU_MESSAGE) == 70 + 40 + 90);
    au_steps(AU_ALERT, &n);
    CHECK(n == 4 && au_total_ms(AU_ALERT) == 4 * 150 + 3 * 60);
    CHECK(au_total_ms(AU_INFO) < au_total_ms(AU_MESSAGE) && au_total_ms(AU_MESSAGE) < au_total_ms(AU_ALERT));
    CHECK(au_total_ms((au_level_t)99) == au_total_ms(AU_INFO)); // unknown = info

    CHECK(au_freq_at(AU_MESSAGE, 0) == 1319 && au_freq_at(AU_MESSAGE, 69) == 1319);
    CHECK(au_freq_at(AU_MESSAGE, 70) == 0 && au_freq_at(AU_MESSAGE, 109) == 0); // gap
    CHECK(au_freq_at(AU_MESSAGE, 110) == 1760 && au_freq_at(AU_MESSAGE, 199) == 1760);
    CHECK(au_freq_at(AU_MESSAGE, 200) == 0 && au_done(AU_MESSAGE, 200) && !au_done(AU_MESSAGE, 199));
    CHECK(au_freq_at(AU_ALERT, 150) == 0 && au_freq_at(AU_ALERT, 210) == 1760 && au_freq_at(AU_ALERT, 420) == 880);
    CHECK(au_freq_at(AU_ALERT, 0xFFFFFFFFu) == 0);

    CHECK(au_for_notice(0) == AU_INFO && au_for_notice(1) == AU_MESSAGE && au_for_notice(2) == AU_ALERT);
    CHECK(au_for_notice(200) == AU_ALERT);
}

static void test_volume_gate(void) {
    CHECK(au_amplitude(0) == 0 && au_amplitude(AU_VOLUME_MAX) == 24000 && au_amplitude(255) == 24000);
    for (uint8_t v = 1; v <= AU_VOLUME_MAX; v++) {
        CHECK(au_amplitude(v) > au_amplitude((uint8_t)(v - 1))); // every step is louder
    }
    CHECK(au_allowed(true, false, 5));
    CHECK(!au_allowed(false, false, 5) && !au_allowed(true, true, 5) && !au_allowed(true, false, 0));

    au_gate_t g = {0};
    CHECK(au_gate_take(&g, 1000, AU_NOTIFY_GAP_MS));
    CHECK(!au_gate_take(&g, 1500, AU_NOTIFY_GAP_MS) && !au_gate_take(&g, 2999, AU_NOTIFY_GAP_MS));
    CHECK(au_gate_take(&g, 3000, AU_NOTIFY_GAP_MS));
    // A refused one does not push the window out
    CHECK(!au_gate_take(&g, 4000, AU_NOTIFY_GAP_MS) && au_gate_take(&g, 5000, AU_NOTIFY_GAP_MS));
    // Across the 32-bit clock wrap
    au_gate_t w = {0};
    CHECK(au_gate_take(&w, 0xFFFFFF00u, AU_NOTIFY_GAP_MS));
    CHECK(!au_gate_take(&w, 100, AU_NOTIFY_GAP_MS)); // 356 ms later
    CHECK(au_gate_take(&w, 2000, AU_NOTIFY_GAP_MS)); // 2256 ms later
}

enum { RATE = 16000, N = 1600 };
static int16_t buf[N + 2];

static void test_square_sine(void) {
    uint32_t ph = 0;
    // Square at 1000 Hz: period 16 samples, +-amplitude, 8 high then 8 low
    au_fill(buf, N, RATE, 1000, 10, AU_SQUARE, &ph);
    int16_t amp = au_amplitude(10);
    CHECK(buf[0] == amp && buf[7] == amp && buf[8] == -amp && buf[15] == -amp && buf[16] == amp);
    for (int i = 0; i < N - 16; i++) {
        CHECK(buf[i] == buf[i + 16]);
    }
    // Sine at 1000 Hz: bounded by the amplitude at every volume, period 16, peak near the amplitude
    for (int vol = 1; vol <= AU_VOLUME_MAX; vol++) {
        int16_t a   = au_amplitude((uint8_t)vol);
        int     max = 0, bad = 0;
        ph = 0;
        au_fill(buf, N, RATE, 1000, (uint8_t)vol, AU_SINE, &ph);
        for (int i = 0; i < N; i++) {
            max = abs(buf[i]) > max ? abs(buf[i]) : max;
            bad += abs(buf[i]) > a;
            bad += buf[i] != buf[i % 16];
        }
        CHECK(bad == 0 && max > a * 95 / 100);
    }
    ph = 0;
    au_fill(buf, 16, RATE, 1000, 10, AU_SINE, &ph);
    CHECK(buf[0] == 0 && buf[4] > 23000 && buf[8] == 0 && buf[12] < -23000 && buf[2] > 0 && buf[10] < 0);
}

static void test_bounds_and_phase(void) {
    // Max volume, both waves, a spread of frequencies (also above Nyquist): never beyond the amplitude
    uint16_t const freqs[] = {1, 440, 1760, 7999, 8000, 20000, 65535};
    for (size_t k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
        for (int w = 0; w < 2; w++) {
            uint32_t ph  = 0x7FFFFFF0u;
            int      bad = 0;
            au_fill(buf, N, RATE, freqs[k], 255, (au_wave_t)w, &ph);
            for (int i = 0; i < N; i++) {
                bad += abs(buf[i]) > 24000;
            }
            CHECK(bad == 0);
        }
    }

    // The phase carries over: two half buffers equal one whole
    static int16_t whole[N], a[N / 2], b[N / 2];
    uint32_t       p1 = 0, p2 = 0;
    au_fill(whole, N, RATE, 1234, 7, AU_SINE, &p1);
    au_fill(a, N / 2, RATE, 1234, 7, AU_SINE, &p2);
    au_fill(b, N / 2, RATE, 1234, 7, AU_SINE, &p2);
    int diff = 0;
    for (int i = 0; i < N / 2; i++) {
        diff += a[i] != whole[i] || b[i] != whole[N / 2 + i];
    }
    CHECK(diff == 0 && p1 == p2);
}

static void test_silence_and_edges(void) {
    uint32_t ph = 5;
    for (int i = 0; i < N + 2; i++) {
        buf[i] = 99;
    }
    au_fill(buf + 1, 0, RATE, 1000, 10, AU_SQUARE, &ph); // empty buffer: nothing touched
    CHECK(buf[1] == 99 && ph == 5);
    au_fill(buf + 1, 8, RATE, 0, 10, AU_SQUARE, &ph); // freq 0
    CHECK(buf[0] == 99 && buf[1] == 0 && buf[8] == 0 && buf[9] == 99);
    for (int i = 1; i <= 8; i++) {
        buf[i] = 99;
    }
    au_fill(buf + 1, 8, RATE, 1000, 0, AU_SINE, &ph); // volume 0
    CHECK(buf[1] == 0 && buf[8] == 0 && buf[9] == 99);
    for (int i = 1; i <= 8; i++) {
        buf[i] = 99;
    }
    au_fill(buf + 1, 8, 0, 1000, 10, AU_SINE, &ph); // rate 0
    CHECK(buf[1] == 0 && buf[8] == 0 && buf[9] == 99);
    ph = 0;
    au_fill(buf + 1, 8, RATE, 1000, 10, AU_SQUARE, &ph); // sound: edges intact
    CHECK(buf[0] == 99 && buf[1] != 0 && buf[8] != 0 && buf[9] == 99);
}

int main(void) {
    test_patterns();
    test_volume_gate();
    test_square_sine();
    test_bounds_and_phase();
    test_silence_and_edges();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("audio_pattern_test: ok\n");
    return 0;
}
