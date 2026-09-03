#include "sstv_modem_ref.h"
#include "sstv_frame.h"

/* sin on [0, pi] without libm (Bhaskara approximation, error < 0.2 %). */
static float sinf_approx(float x) {
    const float pi = 3.14159265f;
    return 16.0f * x * (pi - x) / (5.0f * pi * pi - 4.0f * x * (pi - x));
}

void sstv_mod_init(sstv_mod* m, float sample_rate, int16_t amplitude) {
    static const int tones[SSTV_NUM_TONES] = SSTV_TONES_INIT;
    static const int duty[SSTV_NUM_TONES] = SSTV_TONE_DUTY_PCT_INIT;
    m->sample_rate = sample_rate;
    m->amplitude = amplitude;
    for(int k = 0; k < SSTV_NUM_TONES; k++) {
        m->tones[k] = (float)tones[k];
        m->duty[k] = (float)duty[k] / 100.0f;
        /* fundamental of a pulse train ~ sin(pi * duty); the channel model restores it */
        float f = sinf_approx(3.14159265f * m->duty[k]);
        m->channel_gain[k] = (m->duty[k] > 0.0f && f > 0.05f) ? 1.0f / f : 1.0f;
    }
    m->fsync = (float)SSTV_FSYNC_HZ;
    m->sync_duty = (float)SSTV_SYNC_DUTY_PCT / 100.0f;
    m->phase = 0.0;
    m->time_us = 0.0;
    m->samples_out = 0;
}

int sstv_mod_tone(sstv_mod* m, float freq_hz, int duration_us, int16_t* out, int cap) {
    return sstv_mod_tone_duty(m, freq_hz, 0.5f, duration_us, out, cap);
}

int sstv_mod_tone_duty(sstv_mod* m, float freq_hz, float duty, int duration_us, int16_t* out, int cap) {
    return sstv_mod_tone_gain(m, freq_hz, duty, 1.0f, duration_us, out, cap);
}

int sstv_mod_tone_gain(
    sstv_mod* m, float freq_hz, float duty, float gain, int duration_us, int16_t* out, int cap) {
    double end_us = m->time_us + (double)duration_us;
    long long end_samples = (long long)(end_us * (double)m->sample_rate / 1e6 + 0.5);
    long long n64 = end_samples - m->samples_out;
    if(n64 < 0) n64 = 0;
    if(n64 > cap) return -1;
    int n = (int)n64;
    double step = (double)freq_hz / (double)m->sample_rate;
    for(int i = 0; i < n; i++) {
        if(freq_hz <= 0.0f || duty <= 0.0f) {
            out[i] = 0; /* silence symbol */
            continue;
        }
        float a = (float)m->amplitude * gain;
        if(a > 32767.0f) a = 32767.0f;
        out[i] = m->phase < (double)duty ? (int16_t)a : (int16_t)(-a);
        m->phase += step;
        if(m->phase >= 1.0) m->phase -= 1.0;
    }
    m->time_us = end_us;
    m->samples_out += n;
    return n;
}

int sstv_mod_frame(sstv_mod* m, const uint8_t* frame, int16_t* out, int cap) {
    int total = 0;
    int n = sstv_mod_tone_duty(m, m->fsync, m->sync_duty, SSTV_SYNC_US, out, cap);
    if(n < 0) return -1;
    total += n;
    for(int i = 0; i < SSTV_SYMBOLS_PER_FRAME; i++) {
        int v = sstv_frame_symbol(frame, i);
        n = sstv_mod_tone_gain(m, m->tones[v], m->duty[v], m->channel_gain[v], SSTV_SYMBOL_US, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    return total;
}

int sstv_mod_image(sstv_mod* m, const uint8_t* image_bits, int16_t* out, int cap) {
    int total = 0;
    for(int i = 0; i < SSTV_LEADER_SYMBOLS; i++) {
        int k = i % SSTV_NUM_TONES;
        int n = sstv_mod_tone_gain(m, m->tones[k], m->duty[k], m->channel_gain[k], SSTV_SYMBOL_US, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    static uint8_t phys[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    int frames = sstv_build_physical(image_bits, "image", phys, NULL, NULL);
    for(int p = 0; p < frames; p++) {
        int n = sstv_mod_frame(m, phys + p * SSTV_FRAME_BYTES, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    return total;
}

int sstv_mod_image_capacity(float sample_rate) {
    double us = (double)SSTV_LEADER_SYMBOLS * SSTV_SYMBOL_US + (double)SSTV_MAX_FRAMES * SSTV_PHYS_US;
    return (int)(us * (double)sample_rate / 1e6) + 64;
}
