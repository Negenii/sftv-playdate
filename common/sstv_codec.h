#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lossless per-line coding of 1-bit rows (SSTV_LINE_BYTES each). Three modes, the shortest
 * wins: raw, run lengths of the row's bits, or run lengths of the row XORed with a
 * reference row (the previous line in the same block; zeros for the first). Dithered
 * photographs mostly stay raw; line art, text and flat areas shrink several times.
 * Encoded record: header byte (mode in bits 7..6, payload length 1..63 in bits 5..0) followed
 * by the payload. Raw rows have length SSTV_LINE_BYTES. */
#define SSTV_CODEC_RAW 0
#define SSTV_CODEC_RLE 1
#define SSTV_CODEC_XOR_RLE 2
#define SSTV_CODEC_MAX_RECORD (1 + SSTV_LINE_BYTES)

/* Encodes `row` (ref may be NULL = zeros). Writes at most SSTV_CODEC_MAX_RECORD bytes to out.
 * Returns the record length. */
int sstv_codec_encode(const uint8_t* row, const uint8_t* ref, uint8_t* out);

/* Decodes one record from `in` (at most `avail` bytes). Returns the record length consumed,
 * or -1 if the record is malformed or truncated. */
int sstv_codec_decode(const uint8_t* in, int avail, const uint8_t* ref, uint8_t* row);

#ifdef __cplusplus
}
#endif
