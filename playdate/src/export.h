#pragma once
/* Copies of received items in formats a computer opens (the Playdate itself reads its own .pbm /
 * .sfa): a 1-bit PNG for a picture, an animated GIF for an animation. Output goes through a
 * write callback, so the same code runs on the Playdate and in host tests. */
#include <stdbool.h>
#include <stdint.h>

typedef void (*export_write)(void* ctx, const void* data, int len);

/* Allocator (realloc semantics; size 0 frees). Defaults to the C library's; the Playdate sets
 * pd->system->realloc. */
#include <stddef.h>
extern void* (*export_realloc)(void* ptr, size_t size);

/* row(ctx, y, out): row y as (w + 7) / 8 bytes, leftmost pixel in the top bit, 1 = white. */
typedef void (*export_row)(void* ctx, int y, uint8_t* out);
void export_png(int w, int h, export_row row, void* row_ctx, export_write wr, void* ctx);

/* frames: XBM (stride (w + 7) / 8, leftmost pixel in bit 0, 1 = black), fb bytes each; plays
 * order[0..order_len) at delay_cs hundredths of a second per frame, looping, every pixel scaled
 * by `scale`. Returns false when out of memory. */
bool export_gif(int w, int h, int scale, const uint8_t* frames, int fb, const uint8_t* order, int order_len, int delay_cs,
                export_write wr, void* ctx);
