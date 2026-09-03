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

void sstv_demod_init(sstv_demod* d, float sample_rate, sstv_line_cb cb, void* ctx) {
    table_init();
    memset(d, 0, sizeof *d);
    d->sample_rate = sample_rate;
    d->bit_len = sample_rate * (float)SSTV_BIT_US / 1000000.0f;
    float win = sample_rate * (float)SSTV_WINDOW_US / 1000000.0f;
    d->window = (int)(win + 0.5f);
    if(d->window > SSTV_DEMOD_MAX_WINDOW) d->window = SSTV_DEMOD_MAX_WINDOW;
    if(d->window < 4) d->window = 4;

    const float freqs[3] = {(float)SSTV_F0_HZ, (float)SSTV_F1_HZ, (float)SSTV_FSYNC_HZ};
    for(int k = 0; k < 3; k++) {
        d->det[k].step = (uint32_t)((double)freqs[k] / (double)sample_rate * 4294967296.0);
    }

    d->dominance = 3.0f;
    d->sync_min_run = 3 * d->window; /* 3 ms */
    d->edge_adjust = sample_rate * (float)SSTV_EDGE_ADJUST_US / 1000000.0f;
    d->sample_point = SSTV_SAMPLE_POINT;
    d->gain_decay = 1.0f - 1.0f / (SSTV_GAIN_TAU_S * sample_rate);

    /* A square wave of amplitude A correlated with a Q15 sine over W samples gives
     * |I| ~= A * (4/pi) * (W/2) * 32767. Noise floor: A = 150 LSB. */
    float full = 32767.0f * 0.6366f * (float)d->window * 32767.0f;
    d->level_norm = 1.0f / (full * full);
    float floor_amp = 150.0f * 0.6366f * (float)d->window * 32767.0f;
    d->min_energy = floor_amp * floor_amp;
    d->gain[0] = d->gain[1] = d->min_energy;

    d->state = SSTV_DEMOD_IDLE;
    d->cb = cb;
    d->ctx = ctx;
}

void sstv_demod_reset_counters(sstv_demod* d) {
    d->lines = 0;
    d->crc_errors = 0;
}

static inline void det_push(sstv_det* dt, int16_t new_s, int16_t old_s, int window) {
    uint32_t old_ph = dt->phase - (uint32_t)window * dt->step;
    dt->i += (int64_t)new_s * tcos(dt->phase) - (int64_t)old_s * tcos(old_ph);
    dt->q += (int64_t)new_s * tsin(dt->phase) - (int64_t)old_s * tsin(old_ph);
    dt->phase += dt->step;
}

static void finish_line(sstv_demod* d) {
    uint8_t line_no, pixels[SSTV_LINE_BYTES];
    bool ok = sstv_frame_unpack(d->frame, &line_no, pixels);
    d->lines++;
    if(!ok) d->crc_errors++;
    if(d->cb) d->cb(d->ctx, line_no, pixels, ok);
    d->state = SSTV_DEMOD_IDLE;
}

/* Peak trackers follow each data tone's channel gain so a tone that the speaker or
 * microphone favours does not win every decision. The ratio between them is clamped. */
static inline void track_gains(sstv_demod* d, float e0, float e1) {
    float g0 = d->gain[0] * d->gain_decay;
    float g1 = d->gain[1] * d->gain_decay;
    if(e0 > g0) g0 = e0;
    if(e1 > g1) g1 = e1;
    if(g0 < d->min_energy) g0 = d->min_energy;
    if(g1 < d->min_energy) g1 = d->min_energy;
    if(g1 * SSTV_GAIN_CLAMP < g0) g1 = g0 / SSTV_GAIN_CLAMP;
    if(g0 * SSTV_GAIN_CLAMP < g1) g0 = g1 / SSTV_GAIN_CLAMP;
    d->gain[0] = g0;
    d->gain[1] = g1;
}

void sstv_demod_feed(sstv_demod* d, const int16_t* samples, int n) {
    const int W = d->window;
    for(int idx = 0; idx < n; idx++) {
        int16_t s = samples[idx];
        int16_t old = d->ring[d->ring_pos];
        d->ring[d->ring_pos] = s;
        d->ring_pos++;
        if(d->ring_pos >= W) d->ring_pos = 0;

        for(int k = 0; k < 3; k++) {
            det_push(&d->det[k], s, old, W);
            float fi = (float)d->det[k].i;
            float fq = (float)d->det[k].q;
            d->energy[k] = fi * fi + fq * fq;
        }
        float e0 = d->energy[0], e1 = d->energy[1], es = d->energy[2];
        track_gains(d, e0, e1);
        float edata = e0 > e1 ? e0 : e1;
        bool dominant = (es > d->dominance * edata) && (es > d->min_energy);
        /* Tolerate short dropouts inside a sync burst (noise, echoes): a non-dominant
         * sample costs one, so a sustained burst still accumulates. */
        if(dominant) {
            d->sync_run++;
        } else if(d->sync_run > 0) {
            d->sync_run--;
        }

        switch(d->state) {
        case SSTV_DEMOD_IDLE:
            if(d->sync_run >= d->sync_min_run) d->state = SSTV_DEMOD_SYNC;
            break;

        case SSTV_DEMOD_SYNC:
            /* The window [n-W+1, n] straddles the sync->data edge; energies cross when it
             * is half in each, so the first data sample is about n - W/2. */
            if(es < edata) {
                double t0 = (double)d->n - (double)W / 2.0 + (double)d->edge_adjust;
                d->next_decision = t0 + (double)(d->sample_point * d->bit_len) - 1.0;
                d->bit_index = 0;
                memset(d->frame, 0, sizeof d->frame);
                d->sync_run = 0; /* a resync during DATA needs a fresh burst */
                d->state = SSTV_DEMOD_DATA;
            }
            break;

        case SSTV_DEMOD_DATA:
            if(d->sync_run >= d->sync_min_run) {
                /* A new sync in the middle of a line: abandon it, resync. */
                d->state = SSTV_DEMOD_SYNC;
                break;
            }
            if((double)d->n >= d->next_decision) {
                /* Compare gain-normalised energies over the tail of the symbol.
                 * Ratios, not cross products: energies are ~1e20 and would overflow float. */
                float n1 = e1 / d->gain[1], n0 = e0 / d->gain[0];
                d->margin[d->bit_index] = (n1 - n0) / (n1 + n0 + 1e-30f);
                if(n1 > n0) {
                    d->frame[d->bit_index >> 3] |= (uint8_t)(0x80 >> (d->bit_index & 7));
                }
                d->bit_index++;
                d->next_decision += (double)d->bit_len;
                if(d->bit_index >= SSTV_FRAME_BITS) finish_line(d);
            }
            break;
        }
        d->n++;
    }
}

void sstv_demod_status(const sstv_demod* d, sstv_demod_status_t* out) {
    int best = -1;
    float best_e = d->min_energy;
    for(int k = 0; k < 3; k++) {
        if(d->energy[k] > best_e) {
            best_e = d->energy[k];
            best = k;
        }
    }
    float max_e = d->energy[0];
    if(d->energy[1] > max_e) max_e = d->energy[1];
    if(d->energy[2] > max_e) max_e = d->energy[2];
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
    out->lines = d->lines;
    out->crc_errors = d->crc_errors;
    out->state = d->state;
}
