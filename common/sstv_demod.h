#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_DEMOD_MAX_WINDOW 64 /* supports sample rates up to 64 kHz */
#define SSTV_DEMOD_TABLE_BITS 10
#define SSTV_DEMOD_TABLE_SIZE (1 << SSTV_DEMOD_TABLE_BITS)

typedef void (*sstv_line_cb)(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok);

typedef enum {
    SSTV_DEMOD_IDLE = 0, /* waiting for a sync burst */
    SSTV_DEMOD_SYNC, /* inside a sync burst, waiting for the sync->data crossover */
    SSTV_DEMOD_DATA, /* clocking out 416 bits */
} sstv_demod_state;

/* One quadrature detector: running I/Q sums over a sliding window. */
typedef struct {
    uint32_t phase; /* phase of the next sample to be pushed */
    uint32_t step; /* phase increment per sample (2^32 == one cycle) */
    int64_t i, q; /* running sums in Q15 */
} sstv_det;

typedef struct {
    float sample_rate;
    int window; /* W: samples in one detector window (1 ms) */
    float bit_len; /* samples per symbol, exact */
    float min_energy; /* noise floor on detector energy */
    float dominance; /* sync energy must exceed data energies by this factor */
    int sync_min_run; /* net dominant samples to accept a sync */
    float level_norm; /* energy -> 0..1 level */
    float edge_adjust; /* samples added to the estimated sync->data edge (tuning) */
    float sample_point; /* fraction of the symbol at which the decision window ends */
    float gain_decay; /* per-sample decay of the tone peak trackers */

    sstv_det det[3]; /* 0: F0, 1: F1, 2: FSYNC */
    int16_t ring[SSTV_DEMOD_MAX_WINDOW];
    int ring_pos;
    long long n; /* samples consumed */

    sstv_demod_state state;
    int sync_run;
    double next_decision; /* sample index at which the next bit is decided */
    int bit_index;
    uint8_t frame[SSTV_FRAME_BYTES];

    float energy[3];
    float margin[SSTV_FRAME_BITS]; /* per-bit soft decision of the last line (debug/tuning) */
    float gain[2]; /* tracked peak energy of tone 0 and tone 1 (channel gain) */
    int lines;
    int crc_errors;
    sstv_line_cb cb;
    void* ctx;
} sstv_demod;

typedef struct {
    float level; /* 0..1, strongest detector, amplitude scale */
    int tone; /* -1 none, 0 F0, 1 F1, 2 sync */
    int lines;
    int crc_errors;
    sstv_demod_state state;
} sstv_demod_status_t;

void sstv_demod_init(sstv_demod* d, float sample_rate, sstv_line_cb cb, void* ctx);
void sstv_demod_feed(sstv_demod* d, const int16_t* samples, int n);
void sstv_demod_status(const sstv_demod* d, sstv_demod_status_t* out);
void sstv_demod_reset_counters(sstv_demod* d);

#ifdef __cplusplus
}
#endif
