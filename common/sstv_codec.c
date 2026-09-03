#include "sstv_codec.h"
#include <string.h>

#define ROW_BITS (SSTV_LINE_BYTES * 8)

static inline int get_bit(const uint8_t* row, int i) {
    return (row[i >> 3] >> (7 - (i & 7))) & 1;
}

/* Run lengths of the bits, colours alternating starting with 0. A run of 0 length is legal
 * (used when the row starts with a 1, or to split runs longer than 255). Returns the
 * number of bytes, or a value > limit as soon as the encoding cannot win. */
static int rle_encode(const uint8_t* row, uint8_t* out, int limit) {
    int n = 0, colour = 0, i = 0;
    while(i < ROW_BITS) {
        int run = 0;
        while(i < ROW_BITS && get_bit(row, i) == colour && run < 255) {
            run++;
            i++;
        }
        if(n >= limit) return limit + 1;
        out[n++] = (uint8_t)run;
        if(run == 255) {
            /* keep colour, emit a zero run of the other colour */
            if(n >= limit) return limit + 1;
            out[n++] = 0;
            continue;
        }
        colour ^= 1;
    }
    return n;
}

static int rle_decode(const uint8_t* in, int len, uint8_t* row) {
    memset(row, 0, SSTV_LINE_BYTES);
    int i = 0, colour = 0;
    for(int k = 0; k < len; k++) {
        int run = in[k];
        if(i + run > ROW_BITS) return -1;
        if(colour) {
            for(int b = 0; b < run; b++) row[(i + b) >> 3] |= (uint8_t)(0x80 >> ((i + b) & 7));
        }
        i += run;
        if(run == 255) {
            /* next byte is a zero run of the other colour, colour unchanged after it */
            if(k + 1 >= len || in[k + 1] != 0) return -1;
            k++;
            continue;
        }
        colour ^= 1;
    }
    return i == ROW_BITS ? 0 : -1;
}

int sstv_codec_encode(const uint8_t* row, const uint8_t* ref, uint8_t* out) {
    uint8_t buf[SSTV_LINE_BYTES];
    uint8_t best_mode = SSTV_CODEC_RAW;
    int best_len = SSTV_LINE_BYTES;
    uint8_t cand[64];

    int n = rle_encode(row, cand, 63);
    if(n < best_len) {
        best_len = n;
        best_mode = SSTV_CODEC_RLE;
        memcpy(out + 1, cand, (size_t)n);
    }
    if(ref) {
        for(int i = 0; i < SSTV_LINE_BYTES; i++) buf[i] = row[i] ^ ref[i];
        n = rle_encode(buf, cand, 63);
        if(n < best_len) {
            best_len = n;
            best_mode = SSTV_CODEC_XOR_RLE;
            memcpy(out + 1, cand, (size_t)n);
        }
    }
    if(best_mode == SSTV_CODEC_RAW) memcpy(out + 1, row, SSTV_LINE_BYTES);
    out[0] = (uint8_t)((best_mode << 6) | (best_len & 0x3F));
    if(best_mode == SSTV_CODEC_RAW) out[0] = (uint8_t)(SSTV_CODEC_RAW << 6) | (uint8_t)SSTV_LINE_BYTES;
    return 1 + best_len;
}

int sstv_codec_decode(const uint8_t* in, int avail, const uint8_t* ref, uint8_t* row) {
    if(avail < 1) return -1;
    int mode = in[0] >> 6;
    int len = in[0] & 0x3F;
    if(len == 0 || avail < 1 + len) return -1;
    if(mode == SSTV_CODEC_RAW) {
        if(len != SSTV_LINE_BYTES) return -1;
        memcpy(row, in + 1, SSTV_LINE_BYTES);
        return 1 + len;
    }
    if(rle_decode(in + 1, len, row) < 0) return -1;
    if(mode == SSTV_CODEC_XOR_RLE) {
        if(!ref) return -1;
        for(int i = 0; i < SSTV_LINE_BYTES; i++) row[i] ^= ref[i];
    } else if(mode != SSTV_CODEC_RLE) {
        return -1;
    }
    return 1 + len;
}
