#include "export.h"
#include <stdlib.h>
#include <string.h>

#ifdef TARGET_PLAYDATE
void* (*export_realloc)(void* ptr, size_t size) = NULL; /* set to pd->system->realloc */
#else
void* (*export_realloc)(void* ptr, size_t size) = realloc;
#endif
#define xmalloc(n) export_realloc(NULL, (n))
#define xfree(p) ((p) ? (void)export_realloc((p), 0) : (void)0)

/* ---- PNG: grey 1-bit, zlib with stored (uncompressed) deflate blocks ---- */

static uint32_t crc_table[256];

static uint32_t crc_update(uint32_t c, const uint8_t* p, int n) {
    if(!crc_table[1]) {
        for(uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for(int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    }
    for(int i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* One PNG chunk; the data is produced in pieces, so the CRC runs alongside. */
typedef struct {
    export_write wr;
    void* ctx;
    uint32_t crc;
} chunk;

static void chunk_begin(chunk* c, const char* type, uint32_t len) {
    uint8_t h[8];
    be32(h, len);
    memcpy(h + 4, type, 4);
    c->wr(c->ctx, h, 8);
    c->crc = crc_update(0xFFFFFFFFu, h + 4, 4);
}

static void chunk_data(chunk* c, const uint8_t* p, int n) {
    c->wr(c->ctx, p, n);
    c->crc = crc_update(c->crc, p, n);
}

static void chunk_end(chunk* c) {
    uint8_t t[4];
    be32(t, c->crc ^ 0xFFFFFFFFu);
    c->wr(c->ctx, t, 4);
}

void export_png(int w, int h, export_row row, void* row_ctx, export_write wr, void* ctx) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    wr(ctx, sig, 8);
    chunk c = {wr, ctx, 0};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 1; /* bit depth */
    ihdr[9] = 0; /* greyscale */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk_begin(&c, "IHDR", 13);
    chunk_data(&c, ihdr, 13);
    chunk_end(&c);

    const int rb = (w + 7) / 8, raw = h * (rb + 1);
    const int blocks = (raw + 65534) / 65535;
    chunk_begin(&c, "IDAT", (uint32_t)(2 + raw + 5 * blocks + 4));
    static const uint8_t zhdr[2] = {0x78, 0x01};
    chunk_data(&c, zhdr, 2);
    uint8_t* line = xmalloc((size_t)rb + 1);
    uint32_t a = 1, b = 0; /* Adler-32 */
    int left = raw, in_block = 0, y = 0, at = rb + 1; /* at: position in `line` */
    while(left > 0) {
        if(in_block == 0) {
            int n = left < 65535 ? left : 65535;
            uint8_t bh[5] = {(uint8_t)(n == left ? 1 : 0), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)};
            chunk_data(&c, bh, 5);
            in_block = n;
        }
        if(at == rb + 1) {
            line[0] = 0; /* filter: none */
            row(row_ctx, y++, line + 1);
            at = 0;
        }
        int n = rb + 1 - at;
        if(n > in_block) n = in_block;
        chunk_data(&c, line + at, n);
        for(int i = 0; i < n; i++) {
            a = (a + line[at + i]) % 65521u;
            b = (b + a) % 65521u;
        }
        at += n;
        in_block -= n;
        left -= n;
    }
    xfree(line);
    uint8_t ad[4];
    be32(ad, (b << 16) | a);
    chunk_data(&c, ad, 4);
    chunk_end(&c);
    chunk_begin(&c, "IEND", 0);
    chunk_end(&c);
}

/* ---- GIF: two colours, LZW, NETSCAPE loop ---- */

typedef struct {
    export_write wr;
    void* ctx;
    uint8_t block[256]; /* [len][up to 255 bytes] */
    int nblock;
    uint32_t acc;
    int nacc;
} bitout;

static void bo_byte(bitout* o, uint8_t v) {
    o->block[1 + o->nblock++] = v;
    if(o->nblock == 255) {
        o->block[0] = 255;
        o->wr(o->ctx, o->block, 256);
        o->nblock = 0;
    }
}

static void bo_code(bitout* o, int code, int size) {
    o->acc |= (uint32_t)code << o->nacc;
    o->nacc += size;
    while(o->nacc >= 8) {
        bo_byte(o, (uint8_t)o->acc);
        o->acc >>= 8;
        o->nacc -= 8;
    }
}

static void bo_flush(bitout* o) {
    if(o->nacc > 0) bo_byte(o, (uint8_t)o->acc);
    o->acc = 0;
    o->nacc = 0;
    if(o->nblock > 0) {
        o->block[0] = (uint8_t)o->nblock;
        o->wr(o->ctx, o->block, o->nblock + 1);
        o->nblock = 0;
    }
    uint8_t z = 0;
    o->wr(o->ctx, &z, 1); /* block terminator */
}

#define HASH_SIZE 8192 /* open addressing over (prefix, pixel) -> code, < 4096 entries */

static void lzw_frame(bitout* o, const uint8_t* f, int w, int h, int scale, int32_t* hkey, int16_t* hval) {
    const int min = 2, clear = 1 << min, eoi = clear + 1, stride = (w + 7) / 8;
    int size = min + 1, next = eoi + 1, prefix = -1;
    memset(hkey, 0xFF, sizeof(int32_t) * HASH_SIZE);
    bo_code(o, clear, size);
    for(int y = 0; y < h * scale; y++) {
        const uint8_t* r = f + (y / scale) * stride;
        for(int x = 0; x < w * scale; x++) {
            int sx = x / scale;
            int pix = (r[sx >> 3] >> (sx & 7)) & 1; /* palette: 0 white, 1 black */
            if(prefix < 0) {
                prefix = pix;
                continue;
            }
            int32_t key = (prefix << 1) | pix;
            int slot = (int)(((uint32_t)key * 2654435761u) >> 19) & (HASH_SIZE - 1);
            while(hkey[slot] >= 0 && hkey[slot] != key) slot = (slot + 1) & (HASH_SIZE - 1);
            if(hkey[slot] == key) {
                prefix = hval[slot];
                continue;
            }
            bo_code(o, prefix, size);
            /* as in gif.h (C. Tangora): the new code widens the codes once it needs the
             * extra bit; at 4095 the table restarts */
            hkey[slot] = key;
            hval[slot] = (int16_t)next;
            if(next >= (1 << size)) size++;
            if(next == 4095) {
                bo_code(o, clear, size);
                memset(hkey, 0xFF, sizeof(int32_t) * HASH_SIZE);
                size = min + 1;
                next = eoi + 1;
            } else {
                next++;
            }
            prefix = pix;
        }
    }
    if(prefix >= 0) bo_code(o, prefix, size);
    bo_code(o, eoi, size);
    bo_flush(o);
}

static void le16(uint8_t* p, int v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

bool export_gif(int w, int h, int scale, const uint8_t* frames, int fb, const uint8_t* order, int order_len, int delay_cs,
                export_write wr, void* ctx) {
    int32_t* hkey = xmalloc(sizeof(int32_t) * HASH_SIZE);
    int16_t* hval = xmalloc(sizeof(int16_t) * HASH_SIZE);
    if(!hkey || !hval) {
        xfree(hkey);
        xfree(hval);
        return false;
    }
    const int W = w * scale, H = h * scale;
    uint8_t hd[13 + 6] = {'G', 'I', 'F', '8', '9', 'a'};
    le16(hd + 6, W);
    le16(hd + 8, H);
    hd[10] = 0x80; /* global colour table of 2 entries */
    hd[11] = 0;
    hd[12] = 0;
    hd[13] = hd[14] = hd[15] = 0xFF; /* 0 white */
    hd[16] = hd[17] = hd[18] = 0x00; /* 1 black */
    wr(ctx, hd, sizeof hd);
    static const uint8_t loop[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00};
    wr(ctx, loop, sizeof loop);
    bitout o;
    memset(&o, 0, sizeof o);
    o.wr = wr;
    o.ctx = ctx;
    for(int i = 0; i < order_len; i++) {
        uint8_t gce[8] = {0x21, 0xF9, 0x04, 0x00, 0, 0, 0x00, 0x00};
        le16(gce + 4, delay_cs);
        wr(ctx, gce, 8);
        uint8_t id[11] = {0x2C};
        le16(id + 5, W);
        le16(id + 7, H);
        id[10] = 2; /* LZW minimum code size */
        wr(ctx, id, 11);
        lzw_frame(&o, frames + order[i] * fb, w, h, scale, hkey, hval);
    }
    uint8_t trailer = 0x3B;
    wr(ctx, &trailer, 1);
    xfree(hkey);
    xfree(hval);
    return true;
}
