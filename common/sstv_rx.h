#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block assembler: takes physical frames from the demodulator (raw bytes + per-bit soft
 * decisions), finds interleave blocks, de-interleaves, decodes each logical frame with
 * Reed-Solomon errors-and-erasures, unpacks the coded lines and reports them. */
typedef void (*sstv_line_cb)(void* ctx, uint16_t line, const uint8_t* pixels);

typedef struct {
    uint8_t phys[SSTV_ILEAVE][SSTV_FRAME_BYTES];
    float conf[SSTV_ILEAVE][SSTV_FRAME_BYTES]; /* per-byte confidence 0..1 */
    int count; /* physical frames in the window */
    int frames_in; /* physical frames received */
    int blocks_ok; /* blocks with at least one good logical frame */
    int frames_ok; /* logical frames that passed RS + CRC */
    int frames_bad;
    int lines_out; /* lines delivered */
    int rs_corrected;
    sstv_line_cb cb;
    void* ctx;
} sstv_rx;

void sstv_rx_init(sstv_rx* rx, sstv_line_cb cb, void* ctx);
void sstv_rx_reset_counters(sstv_rx* rx);

/* One demodulated physical frame: SSTV_FRAME_BYTES bytes and SSTV_FRAME_BITS soft values. */
void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft);

#ifdef __cplusplus
}
#endif
