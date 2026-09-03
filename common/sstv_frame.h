#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logical frame: out[SSTV_FRAME_BYTES] = seg index (big endian), SSTV_SEG_BYTES pixels,
 * CRC-8 over those, then Reed-Solomon parity. */
void sstv_frame_pack(uint16_t seg, const uint8_t* pixels, uint8_t* out);

/* Decodes a logical frame. byte_conf (may be NULL): per-byte confidence in [0,1]; the
 * least confident bytes are tried as erasures (0, 2, 4, ... up to SSTV_PARITY_BYTES) until
 * RS + CRC agree. Returns true on success; seg/pixels are filled either way (raw when
 * decoding failed). *corrected (may be NULL): bytes fixed, or -1 when RS gave up. */
bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* seg, uint8_t* pixels, int* corrected);

static inline bool sstv_frame_unpack(const uint8_t* in, uint16_t* seg, uint8_t* pixels) {
    return sstv_frame_unpack_soft(in, NULL, seg, pixels, NULL);
}

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

/* Byte interleaving of SSTV_ILEAVE logical frames into SSTV_ILEAVE physical frames:
 * physical stream position j*SSTV_ILEAVE + i holds byte j of logical frame i. Both
 * buffers are SSTV_ILEAVE * SSTV_FRAME_BYTES. */
void sstv_interleave(const uint8_t* logical, uint8_t* physical);
void sstv_deinterleave(const uint8_t* physical, uint8_t* logical);
void sstv_deinterleave_conf(const float* physical_conf, float* logical_conf);

/* Segment geometry helpers. */
static inline int sstv_seg_line(int seg) {
    return seg / SSTV_SEGS_PER_LINE;
}
static inline int sstv_seg_byte_offset(int seg) {
    return (seg % SSTV_SEGS_PER_LINE) * SSTV_SEG_BYTES;
}

/* Builds every physical frame of an image, already packed and interleaved:
 * out = SSTV_NUM_SEGS * SSTV_FRAME_BYTES bytes, physical frame p at out + p*FRAME_BYTES. */
void sstv_build_physical(const uint8_t* image_bits, uint8_t* out);

#ifdef __cplusplus
}
#endif
