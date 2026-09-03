#include "sstv_demod.h"
#include "sstv_frame.h"
#include <string.h>

/* Q15 sine table built without libm: rotate a unit vector by 2*pi/N using
 * series-expanded sin/cos of the small step angle. */
static int32_t sine_table[SSTV_DEMOD_TABLE_SIZE];
static bool table_ready = false;

static void table_init(void) {
    if(table_ready) return;
    const double two_pi = 6.283185307179586476925286766559;
    double a = two_pi / SSTV_DEMOD_TABLE_SIZE;
    double a2 = a * a;
    double s = a * (1.0 - a2 / 6.0 * (1.0 - a2 / 20.0 * (1.0 - a2 / 42.0)));
    double c = 1.0 - a2 / 2.0 * (1.0 - a2 / 12.0 * (1.0 - a2 / 30.0));
    double x = 1.0, y = 0.0;
    for(int i = 0; i < SSTV_DEMOD_TABLE_SIZE; i++) {
        sine_table[i] = (int32_t)(y * 32767.0 + (y >= 0 ? 0.5 : -0.5));
        double nx = x * c - y * s;
        double ny = x * s + y * c;
        x = nx;
        y = ny;
    }
    table_ready = true;
}

static inline int32_t tsin(uint32_t ph) {
    return sine_table[ph >> (32 - SSTV_DEMOD_TABLE_BITS)];
}

static inline int32_t tcos(uint32_t ph) {
    return sine_table[((ph >> (32 - SSTV_DEMOD_TABLE_BITS)) + SSTV_DEMOD_TABLE_SIZE / 4) &
                      (SSTV_DEMOD_TABLE_SIZE - 1)];
}

void sstv_demod_init(sstv_demod* d, float sample_rate, sstv_phys_cb cb, void* ctx) {
    table_init();
    memset(d, 0, sizeof *d);
    d->sample_rate = sample_rate;
    d->samples_per_ms = (int)(sample_rate / 1000.0f + 0.5f);
    d->bit_len = sample_rate * (float)SSTV_SYMBOL_US / 1000000.0f;
    d->bit_len_nominal = d->bit_len;
    d->last_edge = -1.0;
    d->predicted_edge = -1.0;
    float win = sample_rate * (float)SSTV_WINDOW_US / 1000000.0f;
    d->window = (int)(win + 0.5f);
    if(d->window > SSTV_DEMOD_MAX_WINDOW) d->window = SSTV_DEMOD_MAX_WINDOW;
    if(d->window < 4) d->window = 4;

    static const int tones[SSTV_NUM_TONES] = SSTV_TONES_INIT;
    for(int k = 0; k < SSTV_NUM_TONES; k++) {
        d->det[k].step = (uint32_t)((double)tones[k] / (double)sample_rate * 4294967296.0);
    }
    d->det[SSTV_DEMOD_SYNC_DET].step =
        (uint32_t)((double)SSTV_FSYNC_HZ / (double)sample_rate * 4294967296.0);
    d->sync_long.step = d->det[SSTV_DEMOD_SYNC_DET].step;

    d->dominance = 3.0f;
    d->sync_min_run = SSTV_SYNC_MIN_MS * d->samples_per_ms;
    d->edge_adjust = sample_rate * (float)SSTV_EDGE_ADJUST_US / 1000000.0f;
    d->sample_point = SSTV_SAMPLE_POINT;
    d->gain_decay = 1.0f - 1.0f / (SSTV_GAIN_TAU_S * sample_rate);

    /* A square wave of amplitude A correlated with a Q15 sine over W samples gives
     * |I| ~= A * (4/pi) * (W/2) * 32767. */
    float full = 32767.0f * 0.6366f * (float)d->window * 32767.0f;
    d->level_norm = 1.0f / (full * full);
    float floor_amp = SSTV_NOISE_FLOOR_LSB * 0.6366f * (float)d->window * 32767.0f;
    d->min_energy = floor_amp * floor_amp;
    for(int k = 0; k < SSTV_NUM_TONES; k++) d->gain[k] = d->min_energy;

    d->state = SSTV_DEMOD_IDLE;
    d->cb = cb;
    d->ctx = ctx;
}

void sstv_demod_reset_counters(sstv_demod* d) {
    d->frames = 0;
}

static inline void det_push(sstv_det* dt, int16_t new_s, int16_t old_s, int window) {
    uint32_t old_ph = dt->phase - (uint32_t)window * dt->step;
    dt->i += (int64_t)new_s * tcos(dt->phase) - (int64_t)old_s * tcos(old_ph);
    dt->q += (int64_t)new_s * tsin(dt->phase) - (int64_t)old_s * tsin(old_ph);
    dt->phase += dt->step;
}

static void finish_frame(sstv_demod* d) {
    d->frames++;
    if(d->cb) d->cb(d->ctx, d->frame, d->margin);
    d->state = SSTV_DEMOD_IDLE;
}

/* Peak trackers follow each tone's channel gain so a tone that the speaker or microphone
 * favours does not win every decision. The ratio between tones is clamped. */
static inline void track_gains(sstv_demod* d) {
    float g_min = d->min_energy * 0.01f;
    float gmax = 0.0f;
    for(int k = 0; k < SSTV_NUM_TONES; k++) {
        float g = d->gain[k] * d->gain_decay;
        if(d->energy[k] > g) g = d->energy[k];
        if(g < g_min) g = g_min;
        d->gain[k] = g;
        if(g > gmax) gmax = g;
    }
    float floor = gmax / SSTV_GAIN_CLAMP;
    for(int k = 0; k < SSTV_NUM_TONES; k++) {
        if(d->gain[k] < floor) d->gain[k] = floor;
    }
}

static void decide_symbol(sstv_demod* d) {
    float nrm[SSTV_NUM_TONES];
    int best = 0;
    for(int k = 0; k < SSTV_NUM_TONES; k++) {
        nrm[k] = d->energy[k] / d->gain[k];
        if(nrm[k] > nrm[best]) best = k;
    }
    int base = d->symbol_index * SSTV_BITS_PER_SYMBOL;
    for(int b = 0; b < SSTV_BITS_PER_SYMBOL; b++) {
        int shift = SSTV_BITS_PER_SYMBOL - 1 - b;
        int bit = (best >> shift) & 1;
        if(bit) d->frame[(base + b) >> 3] |= (uint8_t)(0x80 >> ((base + b) & 7));
        /* Soft value: best tone with this bit set vs best tone with it clear. */
        float m1 = 0.0f, m0 = 0.0f;
        for(int k = 0; k < SSTV_NUM_TONES; k++) {
            if((k >> shift) & 1) {
                if(nrm[k] > m1) m1 = nrm[k];
            } else {
                if(nrm[k] > m0) m0 = nrm[k];
            }
        }
        d->margin[base + b] = (m1 - m0) / (m1 + m0 + 1e-30f);
    }
    d->symbol_index++;
}

#define SSTV_FLYWHEEL_FRAMES 3

/* Starts a frame whose first data sample is at t0. real_edge: a sync burst was seen (re-arms
 * the flywheel). */
static void start_frame(sstv_demod* d, double t0, bool real_edge) {
    const double symbols = (double)SSTV_PHYS_US / (double)SSTV_SYMBOL_US;
    if(real_edge) {
        d->flywheel_left = SSTV_FLYWHEEL_FRAMES;
    } else {
        d->flywheel_left--;
        d->flywheel_frames++;
    }
    d->predicted_edge = t0 + symbols * (double)d->bit_len;
    d->next_decision = t0 + (double)(d->sample_point * d->bit_len) - 1.0;
    d->symbol_index = 0;
    memset(d->frame, 0, sizeof d->frame);
    d->sync_run = 0; /* a resync during DATA needs a fresh burst */
    d->state = SSTV_DEMOD_DATA;
}

/* A sync->data crossover was measured at t_meas. Second-order tracking loop: the frame is
 * started between the prediction and the measurement (halving crossover jitter), and the
 * symbol clock is nudged by the remaining error, so both devices' clock offsets and the
 * jitter of the edge estimate are absorbed instead of accumulating. */
static void on_measured_edge(sstv_demod* d, double t_meas) {
    const double symbols = (double)SSTV_PHYS_US / (double)SSTV_SYMBOL_US;
    if(d->predicted_edge < 0.0 || d->flywheel_left <= 0) {
        /* Not locked: take the edge as is; a second edge gives the first clock estimate. */
        if(d->last_edge >= 0.0) {
            double interval = t_meas - d->last_edge;
            double nominal = (double)d->bit_len_nominal * symbols;
            double r = interval / nominal;
            int k = (int)(r + 0.5);
            if(k >= 1 && k <= 4 && r > k * 0.98 && r < k * 1.02) {
                d->bit_len = (float)(interval / (k * symbols));
            }
        }
        d->last_edge = t_meas;
        start_frame(d, t_meas, true);
        return;
    }
    double err = t_meas - d->predicted_edge;
    if(err > 1.5 * (double)d->bit_len || err < -1.5 * (double)d->bit_len) {
        /* Far from where a sync can be: noise shaped like the sync tone. Keep the prediction
         * but treat the burst as seen. */
        start_frame(d, d->predicted_edge, true);
        return;
    }
    double t0 = d->predicted_edge + 0.5 * err;
    d->bit_len += (float)(0.1 * err / symbols);
    d->last_edge = t0;
    start_frame(d, t0, true);
}

/* Flywheel: a sync buried in noise is replaced by the predicted frame start, so only the
 * symbols under the noise are lost (and mostly repaired by the block decoder). */
static void maybe_flywheel(sstv_demod* d) {
    if(d->predicted_edge < 0.0 || d->flywheel_left <= 0) return;
    /* The real crossover shows up W/2 samples after the edge; give it half a symbol. */
    double tolerance = 0.5 * (double)d->bit_len;
    if((double)d->n > d->predicted_edge + tolerance) start_frame(d, d->predicted_edge, false);
}

void sstv_demod_feed(sstv_demod* d, const int16_t* samples, int n) {
    const int W = d->window;
    const int WL = W * SSTV_SYNC_WINDOW_MULT;
    /* A coherent tone's energy scales with window^2: bring the long window to short units. */
    const float long_scale = 1.0f / (float)(SSTV_SYNC_WINDOW_MULT * SSTV_SYNC_WINDOW_MULT);
    for(int idx = 0; idx < n; idx++) {
        int16_t s = samples[idx];
        int pos = d->ring_pos;
        int16_t old = d->ring[(pos - W + SSTV_DEMOD_RING) % SSTV_DEMOD_RING];
        int16_t old_long = d->ring[(pos - WL + SSTV_DEMOD_RING) % SSTV_DEMOD_RING];
        d->ring[pos] = s;
        d->ring_pos = (pos + 1) % SSTV_DEMOD_RING;

        float edata = 0.0f;
        for(int k = 0; k <= SSTV_NUM_TONES; k++) {
            det_push(&d->det[k], s, old, W);
            float fi = (float)d->det[k].i;
            float fq = (float)d->det[k].q;
            d->energy[k] = fi * fi + fq * fq;
            if(k < SSTV_NUM_TONES && d->energy[k] > edata) edata = d->energy[k];
        }
        det_push(&d->sync_long, s, old_long, WL);
        {
            float fi = (float)d->sync_long.i;
            float fq = (float)d->sync_long.q;
            d->energy_sync_long = (fi * fi + fq * fq) * long_scale;
        }
        float es = d->energy[SSTV_DEMOD_SYNC_DET];
        track_gains(d);
        /* Sync acceptance uses the long window (+6 dB SNR); the edge uses the short one. */
        bool dominant = (d->energy_sync_long > d->dominance * edata) &&
                        (d->energy_sync_long > d->min_energy);
        if(dominant) {
            d->sync_run++;
        } else if(d->sync_run > 0) {
            d->sync_run--;
        }

        switch(d->state) {
        case SSTV_DEMOD_IDLE:
            if(d->sync_run >= d->sync_min_run) {
                d->state = SSTV_DEMOD_SYNC;
            } else {
                maybe_flywheel(d);
            }
            break;

        case SSTV_DEMOD_SYNC:
            /* The window [n-W+1, n] straddles the sync->data edge; energies cross when it
             * is half in each, so the first data sample is about n - W/2. */
            if(es < edata) {
                on_measured_edge(d, (double)d->n - (double)W / 2.0 + (double)d->edge_adjust);
            } else {
                maybe_flywheel(d);
            }
            break;

        case SSTV_DEMOD_DATA:
            if(d->sync_run >= d->sync_min_run) {
                /* A sync burst can only legitimately start SSTV_SYNC_US before the predicted
                 * edge; anything earlier is noise that happens to look like the sync tone. */
                double sync_len = (double)d->sample_rate * (double)SSTV_SYNC_US / 1000000.0;
                if(d->predicted_edge >= 0.0 && d->flywheel_left > 0 &&
                   (double)d->n < d->predicted_edge - sync_len - 0.5 * (double)d->bit_len) {
                    d->sync_run = 0;
                } else {
                    d->state = SSTV_DEMOD_SYNC; /* abandon the frame, resync */
                }
                break;
            }
            if((double)d->n >= d->next_decision) {
                decide_symbol(d);
                d->next_decision += (double)d->bit_len;
                if(d->symbol_index >= SSTV_SYMBOLS_PER_FRAME) finish_frame(d);
            }
            break;
        }
        d->n++;
    }
}

void sstv_demod_status(const sstv_demod* d, sstv_demod_status_t* out) {
    int best = -1;
    float best_e = d->min_energy;
    float max_e = 0.0f;
    for(int k = 0; k <= SSTV_NUM_TONES; k++) {
        if(d->energy[k] > best_e) {
            best_e = d->energy[k];
            best = k;
        }
        if(d->energy[k] > max_e) max_e = d->energy[k];
    }
    float lvl = max_e * d->level_norm;
    /* level is an energy ratio; take a cheap square root via Newton steps
     * so the bar is linear in amplitude. */
    if(lvl > 0.0f) {
        float r = lvl < 1.0f ? 0.5f * (lvl + 1.0f) : lvl;
        r = 0.5f * (r + lvl / r);
        r = 0.5f * (r + lvl / r);
        r = 0.5f * (r + lvl / r);
        lvl = r;
    }
    if(lvl > 1.0f) lvl = 1.0f;
    out->level = lvl;
    out->tone = best;
    out->frames = d->frames;
    out->state = d->state;
}
