#include "sstv_anim.h"
#include <string.h>

#define PROB_BITS 12
#define PROB_ONE (1 << PROB_BITS)
#define MOVE_BITS 4
#define TOP (1u << 24)

static inline int px(const uint8_t* f, int stride, int w, int h, int x, int y) {
    if(x < 0 || y < 0 || x >= w || y >= h) return 0;
    return (f[y * stride + (x >> 3)] >> (x & 7)) & 1;
}

/* Context of pixel (x, y): coded neighbours in the frame, the same spot in the reference. */
static inline int context(const uint8_t* f, const uint8_t* r, int stride, int w, int h, int x, int y) {
    int c = px(f, stride, w, h, x - 1, y) | px(f, stride, w, h, x - 2, y) << 1 | px(f, stride, w, h, x - 1, y - 1) << 2 |
            px(f, stride, w, h, x, y - 1) << 3 | px(f, stride, w, h, x + 1, y - 1) << 4 | px(f, stride, w, h, x, y - 2) << 5;
    if(r) {
        c |= px(r, stride, w, h, x, y) << 6 | px(r, stride, w, h, x - 1, y) << 7 | px(r, stride, w, h, x + 1, y) << 8 |
             px(r, stride, w, h, x, y - 1) << 9 | px(r, stride, w, h, x, y + 1) << 10;
    } else {
        c |= 1 << 11; /* key frame */
    }
    return c;
}

uint16_t sstv_anim_crc16(uint16_t crc, uint8_t b) { /* CRC-16/CCITT-FALSE, start 0xFFFF */
    crc ^= (uint16_t)(b << 8);
    for(int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

static int bitcount8(uint8_t v) {
    v = (uint8_t)(v - ((v >> 1) & 0x55));
    v = (uint8_t)((v & 0x33) + ((v >> 2) & 0x33));
    return (v + (v >> 4)) & 0x0F;
}

/* ---- range encoder (LZMA's) ---- */

typedef struct {
    uint64_t low;
    uint32_t range;
    uint8_t cache;
    uint32_t cache_size;
    uint8_t* out;
    int pos, cap;
    bool overflow;
} rc_enc;

static void enc_put(rc_enc* e, uint8_t b) {
    if(e->pos < e->cap) e->out[e->pos] = b;
    else e->overflow = true;
    e->pos++;
}

static void enc_shift_low(rc_enc* e) {
    if((uint32_t)e->low < 0xFF000000u || (uint32_t)(e->low >> 32) != 0) {
        uint8_t carry = (uint8_t)(e->low >> 32);
        uint8_t temp = e->cache;
        do {
            enc_put(e, (uint8_t)(temp + carry));
            temp = 0xFF;
        } while(--e->cache_size != 0);
        e->cache = (uint8_t)((uint32_t)e->low >> 24);
    }
    e->cache_size++;
    e->low = (uint64_t)((uint32_t)e->low & 0x00FFFFFFu) << 8;
}

static void enc_bit(rc_enc* e, uint16_t* p, int bit) {
    uint32_t bound = (e->range >> PROB_BITS) * *p;
    if(!bit) {
        e->range = bound;
        *p = (uint16_t)(*p + ((PROB_ONE - *p) >> MOVE_BITS));
    } else {
        e->low += bound;
        e->range -= bound;
        *p = (uint16_t)(*p - (*p >> MOVE_BITS));
    }
    while(e->range < TOP) {
        e->range <<= 8;
        enc_shift_low(e);
    }
}

static void enc_flush(rc_enc* e) {
    for(int i = 0; i < 5; i++) enc_shift_low(e);
}

int sstv_anim_encode(
    sstv_anim_info* a, sstv_anim_get_frame get, void* ctx, uint8_t* scratch, uint8_t* out, int cap,
    void (*progress)(void* pctx, int frames_done), void* pctx) {
    if(a->w < 1 || a->w > 255 || a->h < 1 || a->h > 255 || a->n < 1 || a->n > SSTV_ANIM_MAX_FRAMES ||
       a->order_len < 0 || a->order_len > SSTV_ANIM_MAX_ORDER)
        return -2;
    for(int i = 0; i < a->order_len; i++)
        if(a->order[i] >= a->n) return -2;
    const int w = a->w, h = a->h, stride = sstv_anim_stride(w), fb = sstv_anim_frame_bytes(a);
    const int hdr = sstv_anim_header_len(a), segs = sstv_anim_segs(a->n);
    if(cap < hdr) return -1;
    uint8_t* slot = scratch; /* ring of the last SSTV_ANIM_SEG frames: frame k in slot k % SEG */
    uint16_t* prob = (uint16_t*)(void*)(scratch + SSTV_ANIM_SEG * fb);
    rc_enc e = {0, 0xFFFFFFFFu, 0, 1, out, hdr, cap, false};
    for(int i = 0; i < SSTV_ANIM_CTX; i++) prob[i] = PROB_ONE / 2;
    for(int k = 0; k < a->n; k++) {
        if(k % SSTV_ANIM_SEG == 0) a->seg_off[k / SSTV_ANIM_SEG] = (uint16_t)(e.pos - hdr); /* CRC checkpoint */
        uint8_t* f = slot + (k % SSTV_ANIM_SEG) * fb;
        if(!get(ctx, k, f)) return -1; /* overwrites frame k - SEG, just out of the window */
        /* reference: an identical frame among the last SEG - 1, else the closest of them */
        int best = -1, best_d = 0x7FFFFFFF;
        for(int j = k - (SSTV_ANIM_SEG - 1) > 0 ? k - (SSTV_ANIM_SEG - 1) : 0; j < k; j++) {
            const uint8_t* g = slot + (j % SSTV_ANIM_SEG) * fb;
            int d = 0;
            for(int b = 0; b < fb && d < best_d; b++) d += bitcount8((uint8_t)(f[b] ^ g[b]));
            if(d < best_d) {
                best_d = d;
                best = j;
            }
        }
        if(best < 0) a->ref[k] = SSTV_ANIM_KEY;
        else if(best_d == 0) a->ref[k] = (uint8_t)(SSTV_ANIM_DUP | best);
        else a->ref[k] = (uint8_t)best;
        if(best_d != 0) {
            const uint8_t* rf = best < 0 ? NULL : slot + (best % SSTV_ANIM_SEG) * fb;
            for(int y = 0; y < h; y++)
                for(int x = 0; x < w; x++) enc_bit(&e, &prob[context(f, rf, stride, w, h, x, y)], px(f, stride, w, h, x, y));
        }
        if(e.overflow) return -1;
        a->frame_end[k] = (uint16_t)(e.pos + 5 < 0xFFFF ? e.pos + 5 : 0xFFFF); /* + the coder's pending bytes */
        if(progress) progress(pctx, k + 1);
    }
    enc_flush(&e);
    if(e.overflow || e.pos - hdr > 0xFFFF) return -1;
    int pos = e.pos;
    for(int sg = 0; sg < segs; sg++) {
        int st = hdr + a->seg_off[sg], en = sg + 1 < segs ? hdr + a->seg_off[sg + 1] : pos;
        uint16_t crc = 0xFFFF;
        for(int i = st; i < en; i++) crc = sstv_anim_crc16(crc, out[i]);
        a->seg_crc[sg] = crc;
    }
    /* header last: ref and seg_off are known now */
    uint8_t* p = out;
    *p++ = SSTV_ANIM_VERSION;
    *p++ = (uint8_t)w;
    *p++ = (uint8_t)h;
    *p++ = (uint8_t)a->n;
    *p++ = (uint8_t)a->fps;
    *p++ = (uint8_t)a->passive;
    *p++ = (uint8_t)a->active;
    *p++ = (uint8_t)a->order_len;
    memcpy(p, a->order, (size_t)a->order_len);
    p += a->order_len;
    memcpy(p, a->ref, (size_t)a->n);
    p += a->n;
    for(int s = 0; s < segs; s++) {
        *p++ = (uint8_t)(a->seg_off[s] & 0xFF);
        *p++ = (uint8_t)(a->seg_off[s] >> 8);
    }
    for(int s = 0; s < segs; s++) {
        *p++ = (uint8_t)(a->seg_crc[s] & 0xFF);
        *p++ = (uint8_t)(a->seg_crc[s] >> 8);
    }
    return pos;
}

/* ---- decoder ---- */

int sstv_anim_parse(sstv_anim_info* a, sstv_anim_byte get, void* ctx) {
    int b[SSTV_ANIM_FIXED];
    for(int i = 0; i < SSTV_ANIM_FIXED; i++) {
        b[i] = get(ctx, i);
        if(b[i] < 0) return 0;
    }
    if(b[0] != SSTV_ANIM_VERSION || b[1] < 1 || b[2] < 1 || b[3] < 1 || b[3] > SSTV_ANIM_MAX_FRAMES) return -1;
    memset(a, 0, sizeof *a);
    a->w = b[1];
    a->h = b[2];
    a->n = b[3];
    a->fps = b[4];
    a->passive = b[5];
    a->active = b[6];
    a->order_len = b[7];
    int len = sstv_anim_header_len(a), pos = SSTV_ANIM_FIXED;
    for(int i = pos; i < len; i++)
        if(get(ctx, i) < 0) return 0;
    for(int i = 0; i < a->order_len; i++) {
        a->order[i] = (uint8_t)get(ctx, pos++);
        if(a->order[i] >= a->n) return -1;
    }
    for(int i = 0; i < a->n; i++) {
        a->ref[i] = (uint8_t)get(ctx, pos++);
        int r = a->ref[i] & 0x7F;
        if(a->ref[i] != SSTV_ANIM_KEY && (r >= i || i - r >= SSTV_ANIM_SEG)) return -1;
    }
    for(int s = 0; s < sstv_anim_segs(a->n); s++) {
        int lo = get(ctx, pos), hi = get(ctx, pos + 1);
        a->seg_off[s] = (uint16_t)(lo | hi << 8);
        pos += 2;
    }
    for(int s = 0; s < sstv_anim_segs(a->n); s++) {
        int lo = get(ctx, pos), hi = get(ctx, pos + 1);
        a->seg_crc[s] = (uint16_t)(lo | hi << 8);
        pos += 2;
    }
    return len;
}

void sstv_anim_dec_init(sstv_anim_dec* d, const sstv_anim_info* a, int hdr_len, int stream_len, uint8_t* frames) {
    memset(d, 0, sizeof *d);
    d->info = *a;
    d->hdr_len = hdr_len;
    d->stream_len = stream_len;
    d->frames = frames;
    memset(frames, 0, (size_t)a->n * (size_t)sstv_anim_frame_bytes(a));
}

void sstv_anim_seg_range(const sstv_anim_dec* d, int s, int* start, int* end) {
    int segs = sstv_anim_segs(d->info.n);
    *start = d->hdr_len + d->info.seg_off[s];
    *end = s + 1 < segs ? d->hdr_len + d->info.seg_off[s + 1] : d->stream_len;
    if(*end > d->stream_len) *end = d->stream_len;
}

int sstv_anim_seg_check(const sstv_anim_dec* d, int s, sstv_anim_byte get, void* ctx) {
    int st, en;
    sstv_anim_seg_range(d, s, &st, &en);
    uint16_t crc = 0xFFFF;
    for(int i = st; i < en; i++) {
        int b = get(ctx, i);
        if(b < 0) return 0;
        crc = sstv_anim_crc16(crc, (uint8_t)b);
    }
    return crc == d->info.seg_crc[s] ? 1 : -1;
}

/* One model runs through the whole stream, so a byte that changed (after a CRC mismatch) means
 * decoding again from the start. The frames keep their pixels meanwhile (no white flash): each
 * is overwritten as it decodes again. */
void sstv_anim_dec_rewind(sstv_anim_dec* d, int s) {
    (void)s;
    d->seg = 0;
    d->seg_open = false;
    d->frame = 0;
    d->frame_open = false;
    d->frames_done = 0;
    memset(d->state, 0, sizeof d->state);
}

static void finish_all(sstv_anim_dec* d) {
    d->seg = sstv_anim_segs(d->info.n); /* sstv_anim_dec_done */
}

/* Stream byte; -2 past the end of the stream: a correct stream never reads there (the coder's
 * flush covers every read), so a wrong byte sent the decoder off course. */
static int stream_byte(const sstv_anim_dec* d, sstv_anim_byte get, void* ctx, int pos) {
    if(pos >= d->stream_len) return -2;
    return get(ctx, pos);
}

int sstv_anim_dec_run(sstv_anim_dec* d, sstv_anim_byte get, void* ctx, int budget, bool final, bool* stuck) {
    const sstv_anim_info* a = &d->info;
    const int w = a->w, h = a->h, stride = sstv_anim_stride(w), fb = sstv_anim_frame_bytes(a);
    const int npx = w * h;
    int done = 0;
    if(stuck) *stuck = false;
    if(sstv_anim_dec_done(d)) return 0;
    if(!d->seg_open) {
        /* the coder's first 5 bytes prime `code` (the first is always 0) */
        int b[5], i;
        d->pos = d->hdr_len;
        d->end = d->stream_len;
        for(i = 0; i < 5; i++) {
            b[i] = stream_byte(d, get, ctx, d->pos + i);
            if(b[i] < 0) break;
        }
        if(i < 5) {
            if(b[i] == -2) d->corrupt++;
            if(final || b[i] == -2) finish_all(d);
            else if(stuck) *stuck = true;
            return 0;
        }
        d->code = 0;
        for(i = 0; i < 5; i++) d->code = (d->code << 8) | (uint32_t)b[i];
        d->pos += 5;
        d->range = 0xFFFFFFFFu;
        for(i = 0; i < SSTV_ANIM_CTX; i++) d->prob[i] = PROB_ONE / 2;
        d->seg_open = true;
        d->frame = 0;
        d->frame_open = false;
    }
    while(budget > 0 && d->frame < a->n) {
        d->seg = d->frame / SSTV_ANIM_SEG;
        uint8_t r = a->ref[d->frame];
        uint8_t* f = d->frames + d->frame * fb;
        if(r & SSTV_ANIM_DUP) {
            memcpy(f, d->frames + (r & 0x7F) * fb, (size_t)fb);
            if(d->row_pos)
                for(int y = 0; y < h; y++) d->row_pos[d->frame * h + y] = (uint16_t)(d->pos > 0xFFFF ? 0xFFFF : d->pos);
            d->state[d->frame] = 2;
            d->frames_done++;
            d->frame++;
            continue;
        }
        const uint8_t* rf = r == SSTV_ANIM_KEY ? NULL : d->frames + r * fb;
        if(!d->frame_open) {
            if(rf) memcpy(f, rf, (size_t)fb);
            else memset(f, 0, (size_t)fb);
            d->pix = 0;
            d->frame_open = true;
            d->state[d->frame] = 1;
        }
        bool blocked = false, corrupt = false;
        while(budget > 0 && d->pix < npx) {
            while(d->range < TOP) {
                int b = stream_byte(d, get, ctx, d->pos);
                if(b == -2) {
                    corrupt = true;
                    break;
                }
                if(b < 0) {
                    blocked = true;
                    break;
                }
                d->range <<= 8;
                d->code = (d->code << 8) | (uint32_t)b;
                d->pos++;
            }
            if(blocked || corrupt) break;
            int x = d->pix % w, y = d->pix / w;
            uint16_t* p = &d->prob[context(f, rf, stride, w, h, x, y)];
            uint32_t bound = (d->range >> PROB_BITS) * *p;
            uint8_t* byte = &f[y * stride + (x >> 3)];
            if(d->code < bound) {
                d->range = bound;
                *p = (uint16_t)(*p + ((PROB_ONE - *p) >> MOVE_BITS));
                *byte &= (uint8_t)~(1u << (x & 7));
            } else {
                d->code -= bound;
                d->range -= bound;
                *p = (uint16_t)(*p - (*p >> MOVE_BITS));
                *byte |= (uint8_t)(1u << (x & 7));
            }
            d->pix++;
            budget--;
            done++;
            if(d->row_pos && d->pix % w == 0) d->row_pos[d->frame * h + d->pix / w - 1] = (uint16_t)(d->pos > 0xFFFF ? 0xFFFF : d->pos);
        }
        if(corrupt) { /* off course: the rest can't be decoded from these bytes */
            d->corrupt++;
            d->state[d->frame] = 3;
            finish_all(d);
            return done;
        }
        if(blocked) {
            if(final) finish_all(d); /* the pass is over: what is missing stays missing */
            else if(stuck) *stuck = true;
            return done;
        }
        if(d->pix >= npx) {
            d->state[d->frame] = 2;
            d->frames_done++;
            d->frame++;
            d->frame_open = false;
        }
    }
    if(d->frame >= a->n) finish_all(d);
    return done;
}
