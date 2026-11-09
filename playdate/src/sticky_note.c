#include "sticky_note.h"
#include <string.h>

static int text_w(PlaydateAPI* pd, LCDFont* f, const char* s, int n) {
    return pd->graphics->getTextWidth(f, s, (size_t)n, kUTF8Encoding, 0);
}

/* Word wrap (hard break at '\n'; a word wider than the line goes on a line of its own) is laid
 * out once per text/width/font and cached as line start/length pairs, so a 64 KB note scrolls
 * without re-measuring every word each frame. */
#define MAX_LINES 6000
static struct {
    const char* text;
    size_t len;
    int width;
    LCDFont* font;
    int lines;
    int start[MAX_LINES];
    int count[MAX_LINES];
} lay;

static void layout(PlaydateAPI* pd, LCDFont* font, int width, const char* text) {
    size_t len = strlen(text);
    if(lay.text == text && lay.len == len && lay.width == width && lay.font == font) return;
    lay.text = text;
    lay.len = len;
    lay.width = width;
    lay.font = font;
    lay.lines = 0;
    int ls = -1, ll = 0; /* current line: start offset and byte count */
    const char* p = text;
    for(;;) {
        while(*p == ' ') p++;
        if(!*p) break;
        if(*p == '\n') {
            if(lay.lines < MAX_LINES) { lay.start[lay.lines] = ls < 0 ? (int)(p - text) : ls; lay.count[lay.lines] = ll; lay.lines++; }
            ls = -1;
            ll = 0;
            p++;
            continue;
        }
        const char* ws = p;
        while(*p && *p != ' ' && *p != '\n') p++;
        int wlen = (int)(p - ws);
        if(ls < 0) {
            ls = (int)(ws - text);
            ll = wlen;
            continue;
        }
        /* candidate: the line so far plus the space and the word (contiguous in the text) */
        int cand = (int)(p - (text + ls));
        if(text_w(pd, font, text + ls, cand) <= width) {
            ll = cand;
        } else {
            if(lay.lines < MAX_LINES) { lay.start[lay.lines] = ls; lay.count[lay.lines] = ll; lay.lines++; }
            ls = (int)(ws - text);
            ll = wlen;
        }
    }
    if(ls >= 0 && lay.lines < MAX_LINES) { lay.start[lay.lines] = ls; lay.count[lay.lines] = ll; lay.lines++; }
}

void draw_sticky_note_scrolled(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text, int* scroll) {
    const int fold = w < 120 ? 8 : 16; /* folded corner size */

    /* paper */
    pd->graphics->fillRect(x, y, w, h, kColorWhite);
    /* border, minus the folded corner on the top-right */
    pd->graphics->drawLine(x, y, x + w - fold, y, 1, kColorBlack); /* top */
    pd->graphics->drawLine(x, y, x, y + h, 1, kColorBlack); /* left */
    pd->graphics->drawLine(x, y + h, x + w, y + h, 1, kColorBlack); /* bottom */
    pd->graphics->drawLine(x + w, y + fold, x + w, y + h, 1, kColorBlack); /* right */
    /* folded corner: diagonal + a small shaded triangle */
    pd->graphics->drawLine(x + w - fold, y, x + w, y + fold, 1, kColorBlack);
    pd->graphics->fillTriangle(x + w - fold, y, x + w, y + fold, x + w - fold, y + fold, kColorBlack);
    pd->graphics->drawLine(x + w - fold, y, x + w - fold, y + fold, 1, kColorWhite);
    pd->graphics->drawLine(x + w - fold, y + fold, x + w, y + fold, 1, kColorWhite);

    pd->graphics->setFont(font);
    int fh = pd->graphics->getFontHeight(font);
    int margin = w < 120 ? 4 : 10;
    int line_h = fh + 2;
    int inner_h = h - 2 * margin;
    bool scrollable = scroll != NULL;
    int text_w_px = w - 2 * margin - (scrollable ? 6 : 0); /* room for the scroll bar */
    layout(pd, font, text_w_px, text);

    int content = lay.lines * line_h;
    int max_scroll = content - inner_h;
    if(max_scroll < 0) max_scroll = 0;
    int off = 0;
    if(scrollable) {
        if(*scroll > max_scroll) *scroll = max_scroll;
        if(*scroll < 0) *scroll = 0;
        off = *scroll;
    }
    int tx = x + margin, top = y + margin;
    pd->graphics->setClipRect(x + 1, top, w - 2, inner_h);
    int first = off / line_h;
    for(int i = first; i < lay.lines; i++) {
        int ty = top + i * line_h - off;
        if(ty > top + inner_h) break;
        if(!scrollable && ty + fh > top + inner_h) break; /* fixed view: whole lines only */
        if(lay.count[i] > 0) pd->graphics->drawText(text + lay.start[i], (size_t)lay.count[i], kUTF8Encoding, tx, ty);
    }
    pd->graphics->clearClipRect();
    if(scrollable && max_scroll > 0) { /* thin bar on the right */
        int bx = x + w - margin - 2, bh = inner_h * inner_h / content;
        if(bh < 8) bh = 8;
        int by = top + (inner_h - bh) * off / max_scroll;
        pd->graphics->fillRect(bx, top, 1, inner_h, kColorBlack);
        pd->graphics->fillRect(bx - 1, by, 3, bh, kColorBlack);
    }
}

int sticky_note_max_scroll(PlaydateAPI* pd, LCDFont* font, int w, int h, const char* text) {
    int margin = w < 120 ? 4 : 10;
    int line_h = pd->graphics->getFontHeight(font) + 2;
    layout(pd, font, w - 2 * margin - 6, text);
    int m = lay.lines * line_h - (h - 2 * margin);
    return m > 0 ? m : 0;
}

void draw_sticky_note(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text) {
    draw_sticky_note_scrolled(pd, font, x, y, w, h, text, NULL);
}
