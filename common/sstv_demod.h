#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_DEMOD_MAX_WINDOW 128 /* 2 ms at up to 64 kHz */
#define SSTV_DEMOD_RING (SSTV_DEMOD_MAX_WINDOW * SSTV_SYNC_WINDOW_MULT)
#define SSTV_DEMOD_TABLE_BITS 10
#define SSTV_DEMOD_TABLE_SIZE (1 << SSTV_DEMOD_TABLE_BITS)
#define SSTV_DEMOD_SYNC_DET SSTV_MAX_TONES /* index of the sync detector */
#define SSTV_DEMOD_HIST 64 /* pattern-sync history, in hops (>= 8 symbols) */

/* One demodulated physical frame: SSTV_FRAME_BYTES raw bytes and SSTV_FRAME_BITS soft
 * decisions in [-1, 1] (positive = 1, magnitude = confidence). Feed it to sstv_rx. */
typedef void (*sstv_phys_cb)(void* ctx, const uint8_t* frame, const float* soft);

typedef enum {
    SSTV_DEMOD_IDLE = 0, /* waiting for a sync burst */
    SSTV_DEMOD_SYNC, /* inside a sync burst, waiting for the sync->data crossover */
    SSTV_DEMOD_DATA, /* clocking out symbols */
} sstv_demod_state;

/* One quadrature detector: running I/Q sums over a sliding window. */
typedef struct {
    uint32_t phase; /* phase of the next sample to be pushed */
    uint32_t step; /* phase increment per sample (2^32 == one cycle) */
    int64_t i, q; /* running sums in Q15 */
} sstv_det;

typedef struct {
    float sample_rate;
    int window; /* W: samples in one detector window (SSTV_WINDOW_US) */
    int samples_per_ms;
    float bit_len; /* samples per symbol, tracked from the sync-to-sync interval */
    float bit_len_nominal; /* samples per symbol at the nominal sample rate */
    double last_edge; /* sample index of the previous accepted sync->data edge, <0 none */
    double predicted_edge; /* where the next edge should be, <0 none (flywheel sync) */
    int flywheel_left; /* frames the flywheel may still start without a real sync */
    int flywheel_frames; /* frames started by prediction (stats) */
    float min_energy; /* noise floor on detector energy */
    float dominance; /* sync energy must exceed data energies by this factor */
    int band; /* SSTV_BAND_AUDIBLE or SSTV_BAND_HF */
    int symbol_us; /* symbol length of the band */
    int num_tones, bits, spf; /* tones (incl. silence), bits per symbol, symbols per frame */
    int first_det; /* first data tone with a detector: 1 when tone 0 is silence, else 0 */
    int lazy; /* data tones computed per window instead of per sample (8-FSK, see block_energies) */
    int lazy_count;
    float edata_lazy; /* strongest data energy at the last refresh */
    float gain_decay_block; /* gain_decay ^ window */
    int sync_min_run; /* net dominant samples to accept a sync */
    float level_norm; /* energy -> 0..1 level */
    float edge_adjust; /* samples added to the estimated sync->data edge (tuning) */
    float sample_point; /* fraction of the symbol at which the decision window ends */
    float gain_decay; /* per-sample decay of the tone peak trackers */

    sstv_det det[SSTV_MAX_TONES + 1]; /* data tones, then sync (at SSTV_DEMOD_SYNC_DET), short window */
    sstv_det sync_long; /* sync with a SSTV_SYNC_WINDOW_MULT x longer window */
    int16_t ring[SSTV_DEMOD_RING];
    int ring_pos;
    long long n; /* samples consumed */

    sstv_demod_state state;
    int sync_run;
    double next_decision; /* sample index at which the next symbol is decided */
    int symbol_index;
    uint8_t frame[SSTV_FRAME_BYTES];

    float energy[SSTV_MAX_TONES + 1];
    float energy_sync_long; /* scaled to the short window's energy units */
    float margin[SSTV_FRAME_BITS]; /* per-bit soft decision of the last line */
    float dbg_nrm[SSTV_SYMBOLS_PER_FRAME][SSTV_MAX_TONES]; /* normalised energies at decision (debug) */
    float gain[SSTV_MAX_TONES]; /* tracked peak energy per tone (channel gain) */
    float silence_level; /* SSTV_SILENCE_LEVEL, adjustable for experiments */
    float sync_gain; /* tracked peak energy of the sync detector */
    float sync_gain_decay;
    float sync_peak; /* peak short-window sync energy within the current burst */
    int frames; /* physical frames delivered */
    /* pattern sync (HF band, revision 13): every `hop` samples the tones' shares of the energy go
     * into share_hist, and the pattern's correlation over the last 8 symbols is checked */
    int pattern;
    int sync_us; /* sync part of a frame (16 ms burst / 24 ms pattern) */
    int hop, hop_count;
    float hop_decay; /* gain decay over one hop */
    float share_hist[SSTV_DEMOD_HIST][SSTV_MAX_TONES];
    int hist_pos;
    float score, score_prev, score_prev2; /* pattern correlation at the last three hops */
    float sync_score; /* correlation of the last accepted pattern */
    int patterns_seen;
    sstv_phys_cb cb;
    void* ctx;
} sstv_demod;

typedef struct {
    float level; /* 0..1, strongest detector, amplitude scale */
    int tone; /* -1 none, 0..num_tones-1 data tone, SSTV_DEMOD_SYNC_DET sync */
    int frames;
    sstv_demod_state state;
} sstv_demod_status_t;

void sstv_demod_init(sstv_demod* d, float sample_rate, sstv_phys_cb cb, void* ctx);
/* Retunes the detectors to SSTV_BAND_AUDIBLE (default) or SSTV_BAND_HF; call right after init. */
void sstv_demod_set_band(sstv_demod* d, int band);
void sstv_demod_feed(sstv_demod* d, const int16_t* samples, int n);
void sstv_demod_status(const sstv_demod* d, sstv_demod_status_t* out);
void sstv_demod_reset_counters(sstv_demod* d);

#ifdef __cplusplus
}
#endif
