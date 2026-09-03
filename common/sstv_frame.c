#include "sstv_frame.h"
#include "crc8.h"
#include "rs.h"
#include <string.h>

void sstv_frame_pack(uint16_t seg, const uint8_t* pixels, uint8_t* out) {
    out[0] = (uint8_t)(seg >> 8);
    out[1] = (uint8_t)(seg & 0xFF);
    memcpy(out + 2, pixels, SSTV_SEG_BYTES);
    out[SSTV_DATA_BYTES - 1] = crc8_compute(out, SSTV_DATA_BYTES - 1);
    rs_encode(out, SSTV_DATA_BYTES, out + SSTV_DATA_BYTES);
}

static bool check(const uint8_t* buf, uint16_t* seg, uint8_t* pixels) {
    *seg = (uint16_t)((buf[0] << 8) | buf[1]);
    memcpy(pixels, buf + 2, SSTV_SEG_BYTES);
    return crc8_compute(buf, SSTV_DATA_BYTES - 1) == buf[SSTV_DATA_BYTES - 1] && *seg < SSTV_NUM_SEGS;
}

bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* seg, uint8_t* pixels, int* corrected) {
    uint8_t buf[SSTV_FRAME_BYTES];
    int order[SSTV_FRAME_BYTES];
    int n_order = 0;
    if(byte_conf) {
        /* Least confident bytes first (insertion sort; the frame is small). */
        for(int i = 0; i < SSTV_FRAME_BYTES; i++) {
            int j = n_order++;
            while(j > 0 && byte_conf[order[j - 1]] > byte_conf[i]) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    int last_c = -1;
    for(int e = 0; e <= (byte_conf ? SSTV_PARITY_BYTES : 0); e += 2) {
        memcpy(buf, in, SSTV_FRAME_BYTES);
        int c = rs_decode_erasures(buf, SSTV_FRAME_BYTES, order, e);
        if(c >= 0) {
            last_c = c;
            if(check(buf, seg, pixels)) {
                if(corrected) *corrected = c;
                return true;
            }
        }
    }
    /* Failed: report the raw content. */
    (void)check(in, seg, pixels);
    if(corrected) *corrected = last_c >= 0 ? -1 : -1;
    return false;
}

void sstv_interleave(const uint8_t* logical, uint8_t* physical) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            physical[j * SSTV_ILEAVE + i] = logical[i * SSTV_FRAME_BYTES + j];
        }
    }
}

void sstv_deinterleave(const uint8_t* physical, uint8_t* logical) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            logical[i * SSTV_FRAME_BYTES + j] = physical[j * SSTV_ILEAVE + i];
        }
    }
}

void sstv_deinterleave_conf(const float* physical_conf, float* logical_conf) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            logical_conf[i * SSTV_FRAME_BYTES + j] = physical_conf[j * SSTV_ILEAVE + i];
        }
    }
}

void sstv_build_physical(const uint8_t* image_bits, uint8_t* out) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    for(int b = 0; b < SSTV_NUM_BLOCKS; b++) {
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            int seg = b * SSTV_ILEAVE + i;
            const uint8_t* px = image_bits + sstv_seg_line(seg) * SSTV_LINE_BYTES + sstv_seg_byte_offset(seg);
            sstv_frame_pack((uint16_t)seg, px, logical + i * SSTV_FRAME_BYTES);
        }
        sstv_interleave(logical, out + b * SSTV_ILEAVE * SSTV_FRAME_BYTES);
    }
}
