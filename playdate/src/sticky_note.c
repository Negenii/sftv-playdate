#include "sticky_note.h"
#include <string.h>

static int text_w(PlaydateAPI* pd, LCDFont* f, const char* s, int n) {
    return pd->graphics->getTextWidth(f, s, (size_t)n, kUTF8Encoding, 0);
}

void draw_sticky_note(PlaydateAPI* pd, LCDFont* font, int x, int y, int w, int h, const char* text) {
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

    /* text, word-wrapped inside the margin */
    pd->graphics->setFont(font);
    int fh = pd->graphics->getFontHeight(font);
    int margin = w < 120 ? 4 : 10;
    int tx = x + margin, ty = y + margin;
    int right = x + w - margin, bottom = y + h - margin;
    int line_h = fh + 2;

    char line[256];
    int ll = 0;
    const char* p = text;
    while(*p && ty + fh <= bottom) {
        while(*p == ' ') p++;
        const char* ws = p;
        while(*p && *p != ' ' && *p != '\n') p++;
        int wlen = (int)(p - ws);
        char cand[256];
        int cl = ll;
        if(cl > 0 && cl < (int)sizeof cand - 1) cand[cl++] = ' ';
        for(int i = 0; i < wlen && cl < (int)sizeof cand - 1; i++) cand[cl++] = ws[i];
        cand[cl] = 0;
        int fits = tx + text_w(pd, font, cand, cl) <= right;
        if(fits || ll == 0) {
            memcpy(line, cand, (size_t)cl + 1);
            ll = cl;
        } else {
            line[ll] = 0;
            pd->graphics->drawText(line, (size_t)ll, kUTF8Encoding, tx, ty);
            ty += line_h;
            ll = 0;
            if(wlen > 0 && ty + fh <= bottom) {
                for(int i = 0; i < wlen && ll < (int)sizeof line - 1; i++) line[ll++] = ws[i];
                line[ll] = 0;
            }
        }
        if(*p == '\n') { /* hard break */
            line[ll] = 0;
            if(ll > 0) pd->graphics->drawText(line, (size_t)ll, kUTF8Encoding, tx, ty);
            ty += line_h;
            ll = 0;
            p++;
        }
    }
    if(ll > 0 && ty + fh <= bottom) {
        line[ll] = 0;
        pd->graphics->drawText(line, (size_t)ll, kUTF8Encoding, tx, ty);
    }
}
