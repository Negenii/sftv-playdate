#pragma once
#include "pd_api.h"

/* Draws a sticky note: white paper rectangle, 1px black border, a folded top-right corner, an
 * inner margin, and the text word-wrapped left-aligned in `font`. Used live during reception
 * and as a gallery thumbnail (pick a smaller w/h and font for the thumbnail). Lines past the
 * bottom margin are clipped. */
void draw_sticky_note(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text);
