#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block assembler: takes physical frames from the demodulator (raw bytes + per-bit soft
 * decisions), finds interleave blocks, de-interleaves, decodes each logical frame with
 * Reed-Solomon errors-and-erasures and reports image data:
 *   pixel_cb: `nbytes` pixel bytes for line `line` starting at byte `x_byte` (a half line in
 *             raw mode, a whole line in coded mode);
 *   meta_cb:  the file name at the start (is_end false) and end (is_end true) of a transmission;
 *   preview_cb: one chunk of the 80x48 preview: `nbytes` bytes at offset chunk * SSTV_PREVIEW_CHUNK
 *             of the SSTV_PREVIEW_BYTES preview bitmap (may be NULL). */
typedef void (*sstv_pixel_cb)(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels);
typedef void (*sstv_meta_cb)(void* ctx, const char* name, bool is_end);
typedef void (*sstv_preview_cb)(void* ctx, int chunk, int nbytes, const uint8_t* data);

typedef struct {
    uint8_t phys[SSTV_ILEAVE][SSTV_FRAME_BYTES];
    float conf[SSTV_ILEAVE][SSTV_FRAME_BYTES]; /* per-byte confidence 0..1 */
    int count; /* physical frames in the window */
    int frames_in; /* physical frames received */
    int blocks_ok; /* blocks with at least one good logical frame */
    int frames_ok; /* logical frames that passed RS + CRC */
    int frames_bad;
    int rs_corrected;
    bool coded; /* last seen data frame type */
    sstv_pixel_cb pixel_cb;
    sstv_meta_cb meta_cb;
    sstv_preview_cb preview_cb;
    void* ctx;
} sstv_rx;

void sstv_rx_init(sstv_rx* rx, sstv_pixel_cb pixel_cb, sstv_meta_cb meta_cb, sstv_preview_cb preview_cb, void* ctx);
void sstv_rx_reset_counters(sstv_rx* rx);

/* One demodulated physical frame: SSTV_FRAME_BYTES bytes and SSTV_FRAME_BITS soft values. */
void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft);

#ifdef __cplusplus
}
#endif
