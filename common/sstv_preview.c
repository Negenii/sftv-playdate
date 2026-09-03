#include "sstv_preview.h"
#include <string.h>

static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* Box-count downscale by (bw x bh) blocks, Bayer-dithered to 1 bit. */
static void shrink(const uint8_t* image_bits, uint8_t* out, int w, int h, int line_bytes, int bw, int bh) {
    memset(out, 0, (size_t)(line_bytes * h));
    int area = bw * bh;
    for(int py = 0; py < h; py++) {
        for(int px = 0; px < w; px++) {
            int black = 0;
            for(int dy = 0; dy < bh; dy++) {
                const uint8_t* row = image_bits + (py * bh + dy) * SSTV_LINE_BYTES;
                for(int dx = 0; dx < bw; dx++) {
                    int x = px * bw + dx;
                    black += (row[x >> 3] >> (7 - (x & 7))) & 1;
                }
            }
            /* black fraction = black / area; threshold (bayer + 0.5) / 16 */
            if(black * 32 > (2 * bayer[py & 3][px & 3] + 1) * area) out[py * line_bytes + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
        }
    }
}

void sstv_preview_make(const uint8_t* image_bits, uint8_t* out) {
    shrink(image_bits, out, SSTV_PREVIEW_W, SSTV_PREVIEW_H, SSTV_PREVIEW_LINE_BYTES, SSTV_WIDTH / SSTV_PREVIEW_W, SSTV_HEIGHT / SSTV_PREVIEW_H);
}

void sstv_preview_make_tiny(const uint8_t* image_bits, uint8_t* out) {
    shrink(image_bits, out, SSTV_TINY_W, SSTV_TINY_H, SSTV_TINY_LINE_BYTES, SSTV_WIDTH / SSTV_TINY_W, SSTV_HEIGHT / SSTV_TINY_H);
}
