#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reed-Solomon over GF(256) (poly 0x11D, alpha = 2), RS_PARITY parity symbols,
 * corrects up to RS_PARITY/2 symbol errors per block. Block = data followed by parity,
 * block length <= 255. */
#define RS_PARITY 12
#define RS_MAX_ERRORS (RS_PARITY / 2)

/* Writes RS_PARITY parity bytes for data[data_len]. */
void rs_encode(const uint8_t* data, int data_len, uint8_t* parity);

/* Corrects block[block_len] (data + parity) in place. Returns the number of corrected
 * symbols (0 if clean) or -1 if the block is uncorrectable. */
int rs_decode(uint8_t* block, int block_len);

#ifdef __cplusplus
}
#endif
