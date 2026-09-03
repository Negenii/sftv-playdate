#include "sstv_rx.h"
#include "sstv_frame.h"
#include <string.h>

void sstv_rx_init(sstv_rx* rx, sstv_line_cb cb, void* ctx) {
    memset(rx, 0, sizeof *rx);
    rx->cb = cb;
    rx->ctx = ctx;
}

void sstv_rx_reset_counters(sstv_rx* rx) {
    rx->frames_in = 0;
    rx->blocks_ok = 0;
    rx->frames_ok = 0;
    rx->frames_bad = 0;
    rx->lines_out = 0;
    rx->rs_corrected = 0;
}

/* Byte confidence = weakest of its bits. */
static void byte_confidence(const float* soft, float* conf) {
    for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
        float c = 1.0f;
        for(int b = 0; b < 8; b++) {
            float m = soft[j * 8 + b];
            if(m < 0) m = -m;
            if(m < c) c = m;
        }
        conf[j] = c;
    }
}

static void line_cb(void* ctx, uint16_t line, const uint8_t* pixels) {
    sstv_rx* rx = ctx;
    rx->lines_out++;
    if(rx->cb) rx->cb(rx->ctx, line, pixels);
}

/* Tries to decode the window as one block. Returns the number of good logical frames. */
static int try_block(sstv_rx* rx) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    float lconf[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(&rx->phys[0][0], logical);
    sstv_deinterleave_conf(&rx->conf[0][0], lconf);

    uint16_t blocks[SSTV_ILEAVE];
    uint8_t payload[SSTV_BLOCK_PAYLOAD];
    bool ok[SSTV_ILEAVE];
    int corr[SSTV_ILEAVE];
    int good = 0;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        ok[i] = sstv_frame_unpack_soft(
            logical + i * SSTV_FRAME_BYTES, lconf + i * SSTV_FRAME_BYTES, &blocks[i],
            payload + i * SSTV_PAYLOAD_BYTES, &corr[i]);
        if(ok[i]) good++;
    }
    if(good == 0) return 0;
    /* All good frames must name the same block. */
    int id = -1;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(!ok[i]) continue;
        if(id >= 0 && blocks[i] != id) return 0;
        id = blocks[i];
    }
    rx->blocks_ok++;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(ok[i]) {
            rx->frames_ok++;
            if(corr[i] > 0) rx->rs_corrected += corr[i];
        } else {
            rx->frames_bad++;
        }
    }
    /* Records are only trusted up to the first failed logical frame. */
    int valid = 0;
    while(valid < SSTV_ILEAVE && ok[valid]) valid++;
    sstv_block_unpack(payload, valid * SSTV_PAYLOAD_BYTES, line_cb, rx);
    return good;
}

void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft) {
    rx->frames_in++;
    if(rx->count == SSTV_ILEAVE) {
        memmove(rx->phys[0], rx->phys[1], sizeof(rx->phys) - sizeof(rx->phys[0]));
        memmove(rx->conf[0], rx->conf[1], sizeof(rx->conf) - sizeof(rx->conf[0]));
        rx->count--;
    }
    memcpy(rx->phys[rx->count], frame, SSTV_FRAME_BYTES);
    byte_confidence(soft, rx->conf[rx->count]);
    rx->count++;
    if(rx->count < SSTV_ILEAVE) return;
    if(try_block(rx) > 0) rx->count = 0; /* block consumed; next one starts fresh */
}
