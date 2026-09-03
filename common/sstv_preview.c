#include "sstv_preview.h"
#include <string.h>

static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void sstv_preview_make(const uint8_t* image_bits, uint8_t* out) {
    memset(out, 0, SSTV_PREVIEW_BYTES);
    for(int py = 0; py < SSTV_PREVIEW_H; py++) {
        for(int px = 0; px < SSTV_PREVIEW_W; px++) {
            int black = 0;
            for(int dy = 0; dy < 5; dy++) {
                const uint8_t* row = image_bits + (py * 5 + dy) * SSTV_LINE_BYTES;
                for(int dx = 0; dx < 5; dx++) {
                    int x = px * 5 + dx;
                    black += (row[x >> 3] >> (7 - (x & 7))) & 1;
                }
            }
            /* black fraction = black / 25; threshold (bayer + 0.5) / 16 */
            if(black * 32 > (2 * bayer[py & 3][px & 3] + 1) * 25) out[py * SSTV_PREVIEW_LINE_BYTES + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
        }
    }
}
