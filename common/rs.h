#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reed-Solomon over GF(256) (poly 0x11D, alpha = 2), RS_PARITY parity symbols,
 * corrects up to RS_PARITY/2 symbol errors per block. Block = data followed by parity,
 * block length <= 255. */
#include "sstv_params.h"
#ifndef RS_PARITY
#define RS_PARITY SSTV_PARITY_BYTES
#endif
#define RS_MAX_ERRORS (RS_PARITY / 2)

/* Writes RS_PARITY parity bytes for data[data_len]. */
void rs_encode(const uint8_t* data, int data_len, uint8_t* parity);

/* Streaming form of rs_encode: feed data bytes one at a time, then read the parity. */
typedef struct {
    uint8_t rem[RS_PARITY];
} rs_encoder_t;
void rs_encoder_init(rs_encoder_t* e);
void rs_encoder_feed(rs_encoder_t* e, uint8_t byte);
void rs_encoder_finish(const rs_encoder_t* e, uint8_t* parity);

/* Corrects block[block_len] (data + parity) in place. Returns the number of corrected
 * symbols (0 if clean) or -1 if the block is uncorrectable. */
int rs_decode(uint8_t* block, int block_len);

/* Errors-and-erasures decoding: erasures[] lists positions known to be unreliable
 * (num_erasures <= RS_PARITY). Corrects when 2 * errors + erasures <= RS_PARITY.
 * Returns the number of corrected symbols (erasures whose value was already right are
 * not counted) or -1. */
int rs_decode_erasures(uint8_t* block, int block_len, const int* erasures, int num_erasures);

#ifdef __cplusplus
}
#endif
