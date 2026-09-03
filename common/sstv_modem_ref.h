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
    float tones[SSTV_NUM_TONES]; /* Hz per symbol value. Initialised from sstv_params. */
    float duty[SSTV_NUM_TONES]; /* PWM duty per tone, 0..1 */
    float fsync;
    float sync_duty;
    double phase; /* cycles, [0,1) */
    double time_us; /* total signal time generated so far */
    long long samples_out; /* total samples generated so far */
} sstv_mod;

void sstv_mod_init(sstv_mod* m, float sample_rate, int16_t amplitude);

/* Appends a square-wave tone with the given duty (0..1; 0.5 = symmetric). Returns samples
 * written, or -1 (nothing written) if cap is too small. */
int sstv_mod_tone_duty(sstv_mod* m, float freq_hz, float duty, int duration_us, int16_t* out, int cap);
int sstv_mod_tone(sstv_mod* m, float freq_hz, int duration_us, int16_t* out, int cap);

/* Sync burst followed by the symbols of one physical frame. */
int sstv_mod_frame(sstv_mod* m, const uint8_t* frame, int16_t* out, int cap);

/* Leader (cycling symbols) followed by every physical frame of a P4 raster (interleaved). */
int sstv_mod_image(sstv_mod* m, const uint8_t* image_bits, int16_t* out, int cap);

/* Samples needed by sstv_mod_image at this sample rate (with a little slack). */
int sstv_mod_image_capacity(float sample_rate);

#ifdef __cplusplus
}
#endif
