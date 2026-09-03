#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Logical frame: out[SSTV_FRAME_BYTES] = block index (big endian), SSTV_PAYLOAD_BYTES payload,
 * CRC-8 over those, then Reed-Solomon parity. */
void sstv_frame_pack(uint16_t block, const uint8_t* payload, uint8_t* out);

/* Decodes a logical frame. byte_conf (may be NULL): per-byte confidence in [0,1]; the
 * least confident bytes are tried as erasures (0, 2, 4, ... up to SSTV_PARITY_BYTES) until
 * RS + CRC agree. Returns true on success; block/payload are filled either way (raw when
 * decoding failed). *corrected (may be NULL): bytes fixed, or -1 when RS gave up. */
bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* block, uint8_t* payload, int* corrected);

static inline bool sstv_frame_unpack(const uint8_t* in, uint16_t* block, uint8_t* payload) {
    return sstv_frame_unpack_soft(in, NULL, block, payload, NULL);
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

/* Block payload: [line count][first line#][codec record]... consecutive lines, zero padded. */

/* Packs lines starting at `first_line` into one block payload (SSTV_BLOCK_PAYLOAD bytes).
 * Returns the number of lines packed (>= 1 while first_line < SSTV_HEIGHT). */
int sstv_block_pack(const uint8_t* image_bits, int first_line, uint8_t* payload);

/* Parses a block payload whose first `valid_bytes` bytes are trustworthy (the rest came
 * from frames that failed). Calls cb for every line fully inside the valid part. Returns
 * the number of lines delivered. */
typedef void (*sstv_block_line_cb)(void* ctx, uint16_t line, const uint8_t* pixels);
int sstv_block_unpack(const uint8_t* payload, int valid_bytes, sstv_block_line_cb cb, void* ctx);

/* Builds every physical frame of an image: coded, packed into blocks, RS-protected and
 * interleaved. out must hold SSTV_MAX_FRAMES * SSTV_FRAME_BYTES bytes. Returns the number
 * of physical frames (a multiple of SSTV_ILEAVE). */
int sstv_build_physical(const uint8_t* image_bits, uint8_t* out);

#ifdef __cplusplus
}
#endif
