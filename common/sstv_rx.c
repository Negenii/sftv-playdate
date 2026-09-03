#include "sstv_rx.h"
#include "sstv_frame.h"
#include <string.h>

void sstv_rx_init(sstv_rx* rx, sstv_pixel_cb pixel_cb, sstv_meta_cb meta_cb, void* ctx) {
    memset(rx, 0, sizeof *rx);
    rx->pixel_cb = pixel_cb;
    rx->meta_cb = meta_cb;
    rx->ctx = ctx;
}

void sstv_rx_reset_counters(sstv_rx* rx) {
    rx->frames_in = 0;
    rx->blocks_ok = 0;
    rx->frames_ok = 0;
    rx->frames_bad = 0;
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

static void coded_line(void* ctx, uint16_t line, const uint8_t* pixels) {
    sstv_rx* rx = ctx;
    if(rx->pixel_cb) rx->pixel_cb(rx->ctx, line, 0, SSTV_LINE_BYTES, pixels);
}

/* Tries to decode the window as one block. Returns the number of good logical frames. */
static int try_block(sstv_rx* rx) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    float lconf[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(&rx->phys[0][0], logical);
    sstv_deinterleave_conf(&rx->conf[0][0], lconf);

    uint16_t hdr[SSTV_ILEAVE];
    uint8_t payload[SSTV_BLOCK_PAYLOAD];
    bool ok[SSTV_ILEAVE];
    int corr[SSTV_ILEAVE];
    int good = 0;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        ok[i] = sstv_frame_unpack_soft(
            logical + i * SSTV_FRAME_BYTES, lconf + i * SSTV_FRAME_BYTES, &hdr[i],
            payload + i * SSTV_PAYLOAD_BYTES, &corr[i]);
        if(ok[i]) good++;
    }
    if(good == 0) return 0;
    /* All good frames must agree on the type (and, for coded blocks, on the block index). */
    int type = -1, index = -1;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(!ok[i]) continue;
        int t = sstv_hdr_type(hdr[i]), x = sstv_hdr_index(hdr[i]);
        if(type >= 0 && t != type) return 0;
        if(t == SSTV_FT_CODED && index >= 0 && x != index) return 0;
        if(t == SSTV_FT_RAW && index >= 0 && x != index + i) return 0;
        type = t;
        index = (t == SSTV_FT_RAW) ? x - i : x;
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
    if(type == SSTV_FT_META) {
        char name[SSTV_PAYLOAD_BYTES];
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            if(!ok[i]) continue;
            const uint8_t* m = payload + i * SSTV_PAYLOAD_BYTES;
            memcpy(name, m + 1, SSTV_PAYLOAD_BYTES - 1);
            name[SSTV_PAYLOAD_BYTES - 1] = 0;
            if(rx->meta_cb) rx->meta_cb(rx->ctx, name, m[0] == SSTV_META_END);
            break;
        }
    } else if(type == SSTV_FT_RAW) {
        rx->coded = false;
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            if(!ok[i]) continue;
            int seg = sstv_hdr_index(hdr[i]);
            if(rx->pixel_cb) {
                rx->pixel_cb(rx->ctx, (uint16_t)sstv_seg_line(seg), sstv_seg_byte_offset(seg), SSTV_SEG_BYTES,
                             payload + i * SSTV_PAYLOAD_BYTES);
            }
        }
    } else {
        rx->coded = true;
        /* Records are only trusted up to the first failed logical frame. */
        int valid = 0;
        while(valid < SSTV_ILEAVE && ok[valid]) valid++;
        sstv_block_unpack(payload, valid * SSTV_PAYLOAD_BYTES, coded_line, rx);
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
    if(try_block(rx) > 0) rx->count = 0; /* block consumed; next one starts fresh */
}
