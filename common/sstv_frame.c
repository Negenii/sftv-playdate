#include "sstv_frame.h"
#include "sstv_codec.h"
#include "crc8.h"
#include "rs.h"
#include <string.h>

void sstv_frame_pack(uint16_t block, const uint8_t* payload, uint8_t* out) {
    out[0] = (uint8_t)(block >> 8);
    out[1] = (uint8_t)(block & 0xFF);
    memcpy(out + 2, payload, SSTV_PAYLOAD_BYTES);
    out[SSTV_DATA_BYTES - 1] = crc8_compute(out, SSTV_DATA_BYTES - 1);
    rs_encode(out, SSTV_DATA_BYTES, out + SSTV_DATA_BYTES);
}

static bool check(const uint8_t* buf, uint16_t* block, uint8_t* payload) {
    *block = (uint16_t)((buf[0] << 8) | buf[1]);
    memcpy(payload, buf + 2, SSTV_PAYLOAD_BYTES);
    return crc8_compute(buf, SSTV_DATA_BYTES - 1) == buf[SSTV_DATA_BYTES - 1] && *block < SSTV_MAX_BLOCKS;
}

bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* block, uint8_t* payload, int* corrected) {
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
    for(int e = 0; e <= (byte_conf ? SSTV_PARITY_BYTES : 0); e += 2) {
        memcpy(buf, in, SSTV_FRAME_BYTES);
        int c = rs_decode_erasures(buf, SSTV_FRAME_BYTES, order, e);
        if(c >= 0 && check(buf, block, payload)) {
            if(corrected) *corrected = c;
            return true;
        }
    }
    (void)check(in, block, payload);
    if(corrected) *corrected = -1;
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

int sstv_block_pack(const uint8_t* image_bits, int first_line, uint8_t* payload) {
    memset(payload, 0, SSTV_BLOCK_PAYLOAD);
    payload[1] = (uint8_t)first_line;
    int pos = 2, count = 0;
    const uint8_t* ref = NULL;
    uint8_t rec[SSTV_CODEC_MAX_RECORD];
    for(int y = first_line; y < SSTV_HEIGHT; y++) {
        const uint8_t* row = image_bits + y * SSTV_LINE_BYTES;
        int n = sstv_codec_encode(row, ref, rec);
        if(pos + n > SSTV_BLOCK_PAYLOAD) break;
        memcpy(payload + pos, rec, (size_t)n);
        pos += n;
        count++;
        ref = row;
    }
    payload[0] = (uint8_t)count;
    return count;
}

int sstv_block_unpack(const uint8_t* payload, int valid_bytes, sstv_block_line_cb cb, void* ctx) {
    if(valid_bytes < 2) return 0;
    int count = payload[0];
    int line = payload[1];
    int pos = 2, delivered = 0;
    uint8_t prev[SSTV_LINE_BYTES], row[SSTV_LINE_BYTES];
    const uint8_t* ref = NULL;
    for(int k = 0; k < count && line + k < SSTV_HEIGHT; k++) {
        if(pos + 1 > valid_bytes) break;
        int n = sstv_codec_decode(payload + pos, SSTV_BLOCK_PAYLOAD - pos, ref, row);
        if(n < 0 || pos + n > valid_bytes) break;
        pos += n;
        if(cb) cb(ctx, (uint16_t)(line + k), row);
        delivered++;
        memcpy(prev, row, SSTV_LINE_BYTES);
        ref = prev;
    }
    return delivered;
}

int sstv_build_physical(const uint8_t* image_bits, uint8_t* out) {
    uint8_t payload[SSTV_BLOCK_PAYLOAD];
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    int line = 0, blocks = 0;
    while(line < SSTV_HEIGHT && blocks < SSTV_MAX_BLOCKS) {
        int n = sstv_block_pack(image_bits, line, payload);
        if(n <= 0) break;
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            sstv_frame_pack((uint16_t)blocks, payload + i * SSTV_PAYLOAD_BYTES, logical + i * SSTV_FRAME_BYTES);
        }
        sstv_interleave(logical, out + blocks * SSTV_ILEAVE * SSTV_FRAME_BYTES);
        line += n;
        blocks++;
    }
    return blocks * SSTV_ILEAVE;
}
