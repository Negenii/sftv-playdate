#include "sstv_modem_ref.h"
#include "sstv_frame.h"

void sstv_mod_init(sstv_mod* m, float sample_rate, int16_t amplitude) {
    m->sample_rate = sample_rate;
    m->amplitude = amplitude;
    m->f0 = (float)SSTV_F0_HZ;
    m->f1 = (float)SSTV_F1_HZ;
    m->fsync = (float)SSTV_FSYNC_HZ;
    m->phase = 0.0;
    m->time_us = 0.0;
    m->samples_out = 0;
}

int sstv_mod_tone(sstv_mod* m, float freq_hz, int duration_us, int16_t* out, int cap) {
    double end_us = m->time_us + (double)duration_us;
    long long end_samples = (long long)(end_us * (double)m->sample_rate / 1e6 + 0.5);
    long long n64 = end_samples - m->samples_out;
    if(n64 < 0) n64 = 0;
    if(n64 > cap) return -1;
    int n = (int)n64;
    double step = (double)freq_hz / (double)m->sample_rate;
    for(int i = 0; i < n; i++) {
        out[i] = m->phase < 0.5 ? m->amplitude : (int16_t)(-m->amplitude);
        m->phase += step;
        if(m->phase >= 1.0) m->phase -= 1.0;
    }
    m->time_us = end_us;
    m->samples_out += n;
    return n;
}

int sstv_mod_frame(sstv_mod* m, const uint8_t* frame, int16_t* out, int cap) {
    int total = 0;
    int n = sstv_mod_tone(m, m->fsync, SSTV_SYNC_US, out, cap);
    if(n < 0) return -1;
    total += n;
    for(int i = 0; i < SSTV_FRAME_BITS; i++) {
        float f = sstv_frame_bit(frame, i) ? m->f1 : m->f0;
        n = sstv_mod_tone(m, f, SSTV_BIT_US, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    return total;
}

int sstv_mod_image(sstv_mod* m, const uint8_t* image_bits, int16_t* out, int cap) {
    int total = 0;
    for(int i = 0; i < SSTV_LEADER_SYMBOLS; i++) {
        int n = sstv_mod_tone(m, (i & 1) ? m->f1 : m->f0, SSTV_BIT_US, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    uint8_t frame[SSTV_FRAME_BYTES];
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        sstv_frame_pack((uint8_t)y, image_bits + y * SSTV_LINE_BYTES, frame);
        int n = sstv_mod_frame(m, frame, out + total, cap - total);
        if(n < 0) return -1;
        total += n;
    }
    return total;
}

int sstv_mod_image_capacity(float sample_rate) {
    double us = (double)SSTV_LEADER_SYMBOLS * SSTV_BIT_US + (double)SSTV_HEIGHT * SSTV_LINE_US;
    return (int)(us * (double)sample_rate / 1e6) + 64;
}
