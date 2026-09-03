#pragma once
#include <stdint.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shrinks a 400x240 1-bit image (PBM packing, 1 = black) to the 80x48 preview sent after the
 * START meta frame: each preview pixel covers a 5x5 block; the block's black count (0..25) is
 * dithered with a 4x4 Bayer matrix so dithered photographs keep their tones instead of
 * turning to noise. `out` receives SSTV_PREVIEW_BYTES, same packing. */
void sstv_preview_make(const uint8_t* image_bits, uint8_t* out);

/* Same for the 40x20 first layer (10x12 blocks); `out` receives SSTV_TINY_BYTES. */
void sstv_preview_make_tiny(const uint8_t* image_bits, uint8_t* out);

#ifdef __cplusplus
}
#endif
