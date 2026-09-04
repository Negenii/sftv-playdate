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
 *   meta_cb:  start (is_end false) or end (is_end true) of a transmission: file name, total
 *             physical frames of the pass (each SSTV_PHYS_US long) and whether it is coded;
 *   preview_cb: one meta data chunk: layer 0 = 40x20 tiny preview (SSTV_TINY_BYTES), layer 1 =
 *             80x48 preview (SSTV_PREVIEW_BYTES), layer 2 = schedule (lines per coded block,
 *             SSTV_MAX_CODED_BLOCKS bytes); `nbytes` bytes at offset chunk * SSTV_PREVIEW_CHUNK
 *             of that layer (may be NULL). */
typedef void (*sstv_pixel_cb)(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels);
typedef void (*sstv_meta_cb)(void* ctx, const char* name, bool is_end, int total_frames, bool coded);
typedef void (*sstv_preview_cb)(void* ctx, int layer, int chunk, int nbytes, const uint8_t* data);
/* control_cb: a transmitter control frame arrived. subtype = SSTV_META_PAUSE or
 * SSTV_META_RESUME; arg = the frame index it carries (paused-at, or resume-from). */
typedef void (*sstv_control_cb)(void* ctx, int subtype, uint32_t arg);

typedef struct {
    uint8_t phys[SSTV_ILEAVE][SSTV_FRAME_BYTES];
    float conf[SSTV_ILEAVE][SSTV_FRAME_BYTES]; /* per-byte confidence 0..1 */
    int count; /* physical frames in the window */
    int frames_in; /* physical frames received */
    int blocks_ok; /* blocks with at least one good logical frame */
    int last_type; /* type of the most recent accepted block (SSTV_FT_*) */
    int frames_ok; /* logical frames that passed RS + CRC */
    int frames_bad;
    int rs_corrected;
    bool coded; /* last seen data frame type */
    /* Outer code (revision 7): every decoded logical frame is kept by data position; a group
     * is repaired from its parity frames when at most SSTV_OUTER_PARITY frames of the group
     * (data + parity) are missing. Positions come from headers, never from frame counting. */
    uint8_t data[SSTV_MAX_DATA_FRAMES][SSTV_PAYLOAD_BYTES];
    uint8_t have[SSTV_MAX_DATA_FRAMES];
    uint8_t parity[SSTV_OUTER_GROUPS_MAX][SSTV_OUTER_PARITY][SSTV_PAYLOAD_BYTES];
    uint8_t have_parity[SSTV_OUTER_GROUPS_MAX][SSTV_OUTER_PARITY];
    uint8_t group_done[SSTV_OUTER_GROUPS_MAX]; /* complete or repaired: no further attempts */
    int data_frames; /* data positions in the pass (from the announced total), 0 = unknown */
    bool pass_open; /* START seen, END not yet: a repeated START block does not reset */
    int frames_repaired;
    int groups_repaired;
    int repairs_failed; /* parity in hand but too many frames missing */
    sstv_pixel_cb pixel_cb;
    sstv_meta_cb meta_cb;
    sstv_preview_cb preview_cb;
    sstv_control_cb control_cb; /* NULL unless registered */
    void* ctx;
} sstv_rx;

void sstv_rx_init(sstv_rx* rx, sstv_pixel_cb pixel_cb, sstv_meta_cb meta_cb, sstv_preview_cb preview_cb, void* ctx);
void sstv_rx_set_control_cb(sstv_rx* rx, sstv_control_cb cb);
void sstv_rx_reset_counters(sstv_rx* rx);

/* Forgets the previous pass's frames (called on START; sstv_rx_init calls it too). */
void sstv_rx_start_pass(sstv_rx* rx);

/* One demodulated physical frame: SSTV_FRAME_BYTES bytes and SSTV_FRAME_BITS soft values. */
void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft);

#ifdef __cplusplus
}
#endif
