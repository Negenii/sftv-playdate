#pragma once
#include <stdint.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reference software modulator: produces the square-wave sample stream that the Flipper
 * speaker emits. Used by host tests and by the WAV tool. Not used on either device. */
typedef struct {
    float sample_rate;
    int16_t amplitude;
    float tones[SSTV_MAX_TONES]; /* Hz per symbol value. Initialised from sstv_params. */
    float duty[SSTV_MAX_TONES]; /* PWM duty per tone, 0..1 */
    float channel_gain[SSTV_MAX_TONES]; /* amplitude gain the acoustic path applies per tone;
                                            default undoes the duty pre-emphasis (models the
                                            buzzer resonance the pre-emphasis was tuned for) */
    float fsync;
    int symbol_us; /* SSTV_SYMBOL_US, or SSTV_HF_SYMBOL_US for the HF band */
    int band, num_tones, bits, spf; /* see sstv_mod_set_band */
    float sync_duty;
    double phase; /* cycles, [0,1) */
    double time_us; /* total signal time generated so far */
    long long samples_out; /* total samples generated so far */
} sstv_mod;

void sstv_mod_init(sstv_mod* m, float sample_rate, int16_t amplitude);

/* Appends a square-wave tone with the given duty (0..1; 0.5 = symmetric). Returns samples
 * written, or -1 (nothing written) if cap is too small. */
/* Switches to SSTV_BAND_HF (8 tones, 3 bits, 3 ms) or back to SSTV_BAND_AUDIBLE. */
void sstv_mod_set_band(sstv_mod* m, int band);
int sstv_mod_tone_duty(sstv_mod* m, float freq_hz, float duty, int duration_us, int16_t* out, int cap);
/* Same with an amplitude gain (channel model). */
int sstv_mod_tone_gain(sstv_mod* m, float freq_hz, float duty, float gain, int duration_us, int16_t* out, int cap);
int sstv_mod_tone(sstv_mod* m, float freq_hz, int duration_us, int16_t* out, int cap);

/* Sync burst followed by the symbols of one physical frame. */
int sstv_mod_frame(sstv_mod* m, const uint8_t* frame, int16_t* out, int cap);

/* Leader (cycling symbols) followed by every physical frame of a P4 raster (interleaved). */
/* Start preamble: tone ladder + lock frames (sstv_params.h). Returns samples or -1. */
int sstv_mod_preamble(sstv_mod* m, int16_t* out, int cap);
int sstv_mod_image(sstv_mod* m, const uint8_t* image_bits, int16_t* out, int cap);

/* Samples needed by sstv_mod_image at this sample rate (with a little slack). */
int sstv_mod_image_capacity(float sample_rate);

#ifdef __cplusplus
}
#endif
