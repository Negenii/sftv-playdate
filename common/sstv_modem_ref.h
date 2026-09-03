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
    float f0, f1, fsync; /* Hz. Initialised from sstv_params; tests may perturb. */
    double phase; /* cycles, [0,1) */
    double time_us; /* total signal time generated so far */
    long long samples_out; /* total samples generated so far */
} sstv_mod;

void sstv_mod_init(sstv_mod* m, float sample_rate, int16_t amplitude);

/* Appends a tone. Returns samples written, or -1 (nothing written) if cap is too small. */
int sstv_mod_tone(sstv_mod* m, float freq_hz, int duration_us, int16_t* out, int cap);

/* Sync burst followed by the 416 bits of a packed frame. */
int sstv_mod_frame(sstv_mod* m, const uint8_t* frame, int16_t* out, int cap);

/* Leader (alternating 0/1 symbols) followed by all SSTV_HEIGHT lines of a P4 raster (SSTV_IMAGE_BYTES). */
int sstv_mod_image(sstv_mod* m, const uint8_t* image_bits, int16_t* out, int cap);

/* Samples needed by sstv_mod_image at this sample rate (with a little slack). */
int sstv_mod_image_capacity(float sample_rate);

#ifdef __cplusplus
}
#endif
