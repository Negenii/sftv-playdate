#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* out: SSTV_FRAME_BYTES (data + CRC + RS parity). pixels: SSTV_LINE_BYTES. */
void sstv_frame_pack(uint8_t line_no, const uint8_t* pixels, uint8_t* out);

/* Runs Reed-Solomon correction on a copy of `in`, then checks the CRC. Returns true when
 * the CRC matches after correction. line_no and pixels are filled either way (from the
 * corrected copy when RS succeeded, raw otherwise). *corrected (may be NULL) receives the
 * number of corrected bytes, or -1 when RS gave up. */
bool sstv_frame_unpack_ex(const uint8_t* in, uint8_t* line_no, uint8_t* pixels, int* corrected);
bool sstv_frame_unpack(const uint8_t* in, uint8_t* line_no, uint8_t* pixels);

/* Bit `index` (0..SSTV_FRAME_BITS-1) of a packed frame, MSB first within each byte. */
static inline int sstv_frame_bit(const uint8_t* frame, int index) {
    return (frame[index >> 3] >> (7 - (index & 7))) & 1;
}

/* Symbol `index` (0..SSTV_SYMBOLS_PER_FRAME-1): SSTV_BITS_PER_SYMBOL consecutive bits,
 * first bit most significant. */
static inline int sstv_frame_symbol(const uint8_t* frame, int index) {
    int v = 0;
    for(int b = 0; b < SSTV_BITS_PER_SYMBOL; b++) {
        v = (v << 1) | sstv_frame_bit(frame, index * SSTV_BITS_PER_SYMBOL + b);
    }
    return v;
}

#ifdef __cplusplus
}
#endif
