#pragma once
#include "pd_api.h"

/* Draws a sticky note: white paper rectangle, 1px black border, a folded top-right corner, an
 * inner margin, and the text word-wrapped left-aligned in `font`. Used live during reception
 * and as a gallery thumbnail (pick a smaller w/h and font for the thumbnail). Lines past the
 * bottom margin are clipped. */
void draw_sticky_note(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text);

/* Same, scrolled by *scroll pixels (clamped in place), with a scroll bar when the text is longer
 * than the note. The word wrap is cached, so long notes (up to 64 KB) scroll cheaply. */
void draw_sticky_note_scrolled(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text, int* scroll);

/* How far a scrolled note of this size can scroll (0 when the text fits). */
int sticky_note_max_scroll(PlaydateAPI* pd, LCDFont* font, int w, int h, const char* text);
