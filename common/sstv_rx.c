#include "sstv_rx.h"
#include "sstv_frame.h"
#include <string.h>

void sstv_rx_init(sstv_rx* rx, sstv_seg_cb cb, void* ctx) {
    memset(rx, 0, sizeof *rx);
    rx->cb = cb;
    rx->ctx = ctx;
}

void sstv_rx_reset_counters(sstv_rx* rx) {
    rx->frames_in = 0;
    rx->blocks_ok = 0;
    rx->segs_ok = 0;
    rx->segs_bad = 0;
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

/* Tries to decode the window as one block. Returns the number of good segments. */
static int try_block(sstv_rx* rx, bool deliver) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    float lconf[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(&rx->phys[0][0], logical);
    sstv_deinterleave_conf(&rx->conf[0][0], lconf);

    uint16_t segs[SSTV_ILEAVE];
    uint8_t pixels[SSTV_ILEAVE][SSTV_SEG_BYTES];
    bool ok[SSTV_ILEAVE];
    int corr[SSTV_ILEAVE];
    int good = 0;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        ok[i] = sstv_frame_unpack_soft(
            logical + i * SSTV_FRAME_BYTES, lconf + i * SSTV_FRAME_BYTES, &segs[i], pixels[i], &corr[i]);
        if(ok[i]) good++;
    }
    if(good == 0) return 0;
    /* Good segments must agree on the block: seg = base + i. */
    int base = -1;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(!ok[i]) continue;
        int b = segs[i] - i;
        if(b < 0 || b % SSTV_ILEAVE != 0 || (base >= 0 && b != base)) return 0;
        base = b;
    }
    if(!deliver) return good;
    rx->blocks_ok++;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(ok[i]) {
            rx->segs_ok++;
            if(corr[i] > 0) rx->rs_corrected += corr[i];
            if(rx->cb) rx->cb(rx->ctx, segs[i], pixels[i], true, corr[i]);
        } else {
            rx->segs_bad++;
            /* The failed frame's place in the block is known from the good ones. */
            if(rx->cb) rx->cb(rx->ctx, (uint16_t)(base + i), pixels[i], false, -1);
        }
    }
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
    if(try_block(rx, true) > 0) rx->count = 0; /* block consumed; next one starts fresh */
}
