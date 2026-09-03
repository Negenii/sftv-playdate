#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block assembler: takes physical frames from the demodulator (raw bytes + per-bit soft
 * decisions), finds interleave blocks, de-interleaves, decodes each logical frame with
 * Reed-Solomon errors-and-erasures, and reports segments. */
typedef void (*sstv_seg_cb)(void* ctx, uint16_t seg, const uint8_t* pixels, bool ok, int corrected);

typedef struct {
    uint8_t phys[SSTV_ILEAVE][SSTV_FRAME_BYTES];
    float conf[SSTV_ILEAVE][SSTV_FRAME_BYTES]; /* per-byte confidence 0..1 */
    int count; /* physical frames in the window */
    int frames_in; /* physical frames received */
    int blocks_ok; /* blocks with at least one good segment */
    int segs_ok;
    int segs_bad;
    int rs_corrected;
    sstv_seg_cb cb;
    void* ctx;
} sstv_rx;

void sstv_rx_init(sstv_rx* rx, sstv_seg_cb cb, void* ctx);
void sstv_rx_reset_counters(sstv_rx* rx);

/* One demodulated physical frame: SSTV_FRAME_BYTES bytes and SSTV_FRAME_BITS soft values. */
void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft);

#ifdef __cplusplus
}
#endif
