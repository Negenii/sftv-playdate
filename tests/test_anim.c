#include "sstv_anim.h"
#include "sstv_frame.h"
#include "sstv_rx.h"
#include "sstv_demod.h"
#include "sstv_modem_ref.h"
#include <stdio.h>
#include "test_util.h"
#include <string.h>
#include <stdlib.h>

/* Synthetic 128x64 animation: a ball bouncing over a fixed ground line, a blinking box, and
 * frame 9 repeating frame 3 (a duplicate). */
#define W 128
#define H 64
#define N 40
#define FB (((W + 7) / 8) * H)

static uint8_t src[N][FB];

static void set(uint8_t* f, int x, int y) {
    if(x >= 0 && y >= 0 && x < W && y < H) f[y * (W / 8) + (x >> 3)] |= (uint8_t)(1u << (x & 7));
}

static void make_frames(void) {
    memset(src, 0, sizeof src);
    for(int k = 0; k < N; k++) {
        int t = k == 9 ? 3 : k;
        uint8_t* f = src[k];
        for(int x = 0; x < W; x++) set(f, x, 58);
        int cx = 10 + t * 3, cy = 50 - (t % 10) * (10 - t % 10) / 2;
        for(int y = -6; y <= 6; y++)
            for(int x = -6; x <= 6; x++)
                if(x * x + y * y <= 36 && x * x + y * y >= 16) set(f, cx + x, cy + y);
        if(t % 4 < 2)
            for(int y = 5; y < 15; y++)
                for(int x = 100; x < 120; x++) set(f, x, y);
    }
}

static bool get_frame(void* ctx, int i, uint8_t* bits) {
    (void)ctx;
    memcpy(bits, src[i], FB);
    return true;
}

static uint8_t stream[64 * 1024];
static int stream_len;
static uint8_t* avail; /* per byte: arrived */

static int get_byte(void* ctx, int pos) {
    (void)ctx;
    if(pos < 0 || pos >= stream_len || !avail[pos]) return -1;
    return stream[pos];
}

static sstv_anim_info info;

static int encode(void) {
    memset(&info, 0, sizeof info);
    info.w = W;
    info.h = H;
    info.n = N;
    info.fps = 4;
    info.passive = 10;
    info.active = 30;
    info.order_len = N + 2;
    for(int i = 0; i < N; i++) info.order[i] = (uint8_t)i;
    info.order[N] = 3;
    info.order[N + 1] = 2;
    uint8_t* scratch = malloc((size_t)sstv_anim_scratch_bytes(&info));
    int len = sstv_anim_encode(&info, get_frame, NULL, scratch, stream, sizeof stream, NULL, NULL);
    free(scratch);
    return len;
}

static void test_roundtrip(void) {
    stream_len = encode();
    CHECK(stream_len > 0);
    CHECK(stream_len < N * FB / 4); /* simple shapes compress well */
    CHECK_EQ_INT(info.ref[0], SSTV_ANIM_KEY);
    for(int i = 1; i < N; i++) CHECK(info.ref[i] != SSTV_ANIM_KEY); /* only frame 0 is coded alone */
    CHECK_EQ_INT(info.ref[9], SSTV_ANIM_DUP | 3);
    avail = malloc((size_t)stream_len);
    memset(avail, 1, (size_t)stream_len);
    sstv_anim_info in;
    int hl = sstv_anim_parse(&in, get_byte, NULL);
    CHECK_EQ_INT(hl, sstv_anim_header_len(&info));
    CHECK_EQ_INT(in.order_len, N + 2);
    CHECK_EQ_INT(in.order[N], 3);
    CHECK_EQ_INT(in.fps, 4);
    static sstv_anim_dec d;
    uint8_t* frames = malloc((size_t)N * FB);
    sstv_anim_dec_init(&d, &in, hl, stream_len, frames);
    sstv_anim_dec_run(&d, get_byte, NULL, 1 << 30, false, NULL);
    CHECK(sstv_anim_dec_done(&d));
    CHECK_EQ_INT(d.frames_done, N);
    CHECK(memcmp(frames, src, sizeof src) == 0);
    free(frames);
    free(avail);
}

/* Bytes arrive in 25-byte chunks in a shuffled order: the decoder only ever runs on what it
 * has, stops on a gap, resumes, and ends identical. */
static void test_progressive(void) {
    stream_len = encode();
    avail = calloc((size_t)stream_len, 1);
    int chunks = (stream_len + 24) / 25;
    int* order = malloc(sizeof(int) * (size_t)chunks);
    for(int i = 0; i < chunks; i++) order[i] = i;
    srand(7);
    for(int i = 0; i < chunks; i++) { /* mostly in order, with local swaps */
        int j = i + rand() % 4;
        if(j < chunks) {
            int t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
    }
    sstv_anim_info in;
    static sstv_anim_dec d;
    uint8_t* frames = malloc((size_t)N * FB);
    bool parsed = false;
    int stuck_runs = 0;
    for(int c = 0; c < chunks; c++) {
        for(int k = 0; k < 25 && order[c] * 25 + k < stream_len; k++) avail[order[c] * 25 + k] = 1;
        if(!parsed) {
            int hl = sstv_anim_parse(&in, get_byte, NULL);
            CHECK(hl >= 0);
            if(hl > 0) {
                sstv_anim_dec_init(&d, &in, hl, stream_len, frames);
                parsed = true;
            }
        }
        if(parsed) {
            bool stuck;
            sstv_anim_dec_run(&d, get_byte, NULL, 1 << 30, false, &stuck);
            if(stuck) stuck_runs++;
        }
    }
    CHECK(parsed);
    CHECK(stuck_runs > 0);
    CHECK(sstv_anim_dec_done(&d));
    CHECK(memcmp(frames, src, sizeof src) == 0);
    /* a small budget resumes mid-frame */
    sstv_anim_dec_init(&d, &in, sstv_anim_header_len(&in), stream_len, frames);
    int runs = 0;
    while(!sstv_anim_dec_done(&d) && runs < 100000) {
        sstv_anim_dec_run(&d, get_byte, NULL, 777, false, NULL);
        runs++;
    }
    CHECK(memcmp(frames, src, sizeof src) == 0);
    free(order);
    free(frames);
    free(avail);
}

/* A hole that never fills: until the pass ends the decoder waits on it; with final it stops
 * there, the frames before the hole complete. */
static void test_hole(void) {
    stream_len = encode();
    avail = malloc((size_t)stream_len);
    memset(avail, 1, (size_t)stream_len);
    int hl = sstv_anim_header_len(&info);
    int hole = hl + info.seg_off[1] + 40; /* inside checkpoint 1 */
    for(int k = 0; k < 25; k++) avail[hole + k] = 0;
    sstv_anim_info in;
    CHECK_EQ_INT(sstv_anim_parse(&in, get_byte, NULL), hl);
    static sstv_anim_dec d;
    uint8_t* frames = malloc((size_t)N * FB);
    sstv_anim_dec_init(&d, &in, hl, stream_len, frames);
    bool stuck;
    sstv_anim_dec_run(&d, get_byte, NULL, 1 << 30, false, &stuck);
    CHECK(stuck);
    CHECK(!sstv_anim_dec_done(&d));
    CHECK(d.frames_done >= 16);
    CHECK(memcmp(frames, src, 16 * FB) == 0);
    sstv_anim_dec_run(&d, get_byte, NULL, 1 << 30, true, &stuck);
    CHECK(sstv_anim_dec_done(&d));
    CHECK(d.frames_done < N);
    CHECK_EQ_INT(sstv_anim_seg_check(&d, 0, get_byte, NULL), 1);
    CHECK_EQ_INT(sstv_anim_seg_check(&d, 1, get_byte, NULL), 0);
    free(frames);
    free(avail);
}

/* The whole chain: stream -> animation pass -> receiver (12 frames lost, repaired by the outer
 * code) -> bytes from the receiver's store -> decoder. */
static sstv_rx rx;
static bool anim_seen;
static void on_meta(void* c, const char* name, bool is_end, int total, bool coded) {
    (void)c; (void)total; (void)coded;
    if(!is_end) {
        anim_seen = rx.anim && rx.note;
        CHECK(strcmp(name, "Bouncer") == 0 || strcmp(name, "Other") == 0);
    }
}
static int rx_byte(void* ctx, int pos) {
    (void)ctx;
    int c = pos / SSTV_NOTE_CHUNK, at = SSTV_ILEAVE + c;
    if(pos >= rx.note_len || at >= SSTV_RX_MAX_DATA_FRAMES || !rx.have[at]) return -1;
    return rx.data[at][pos % SSTV_NOTE_CHUNK];
}
static void test_pass(void) {
    stream_len = encode();
    int frames = sstv_blob_frames(stream_len);
    uint8_t* out = malloc((size_t)SSTV_MAX_FRAMES * SSTV_FRAME_BYTES); /* room for the note below too */
    CHECK_EQ_INT(sstv_build_anim(stream, stream_len, "Bouncer", out, NULL), frames);
    /* block-at-a-time builder (the Flipper's) gives the same bytes */
    {
        sstv_blob b;
        uint8_t* par = malloc((size_t)sstv_blob_parity_bytes(stream_len));
        sstv_blob_init(&b, stream, stream_len, true, "Bouncer", par);
        CHECK_EQ_INT(b.total, frames);
        uint8_t blk[SSTV_ILEAVE * SSTV_FRAME_BYTES];
        int diff = 0;
        for(int k = 0; k < frames / SSTV_ILEAVE; k++) {
            sstv_blob_block(&b, k, blk);
            if(memcmp(blk, out + k * SSTV_ILEAVE * SSTV_FRAME_BYTES, sizeof blk)) diff++;
        }
        CHECK_EQ_INT(diff, 0);
        /* and for a note */
        static char text[3000];
        for(int i = 0; i < 2999; i++) text[i] = (char)('a' + i % 26);
        int nf = sstv_build_note(text, "n", out, NULL);
        uint8_t* par2 = malloc((size_t)sstv_blob_parity_bytes(2999));
        sstv_blob_init(&b, (const uint8_t*)text, 2999, false, "n", par2);
        CHECK_EQ_INT(b.total, nf);
        diff = 0;
        for(int k = 0; k < nf / SSTV_ILEAVE; k++) {
            sstv_blob_block(&b, k, blk);
            if(memcmp(blk, out + k * SSTV_ILEAVE * SSTV_FRAME_BYTES, sizeof blk)) { diff++; fprintf(stderr, "note block %d/%d differs\n", k, nf / SSTV_ILEAVE); }
        }
        CHECK_EQ_INT(diff, 0);
        free(par);
        free(par2);
        CHECK_EQ_INT(sstv_build_anim(stream, stream_len, "Bouncer", out, NULL), frames);
    }
    sstv_rx_init(&rx, NULL, on_meta, NULL, NULL);
    float soft[SSTV_FRAME_BITS];
    for(int i = 0; i < SSTV_FRAME_BITS; i++) soft[i] = 1.0f;
    uint8_t bad[SSTV_FRAME_BYTES];
    for(int f = 0; f < frames; f++) {
        if(f >= 8 && f < 20) { /* 3 blocks of garbage */
            for(int i = 0; i < SSTV_FRAME_BYTES; i++) bad[i] = (uint8_t)(i * 37 + f);
            sstv_rx_phys(&rx, bad, soft);
        } else {
            sstv_rx_phys(&rx, out + f * SSTV_FRAME_BYTES, soft);
        }
    }
    CHECK(anim_seen);
    CHECK_EQ_INT(rx.note_len, stream_len);
    CHECK(rx.frames_repaired >= 12);
    sstv_anim_info in;
    int hl = sstv_anim_parse(&in, rx_byte, NULL);
    CHECK(hl > 0);
    static sstv_anim_dec d;
    uint8_t* fr = malloc((size_t)N * FB);
    sstv_anim_dec_init(&d, &in, hl, rx.note_len, fr);
    sstv_anim_dec_run(&d, rx_byte, NULL, 1 << 30, true, NULL);
    CHECK_EQ_INT(d.frames_done, N);
    CHECK(memcmp(fr, src, sizeof src) == 0);
    free(fr);
    free(out);
}

/* Through sound: the Flipper's block-at-a-time pass, HF modulation with noise, the demodulator,
 * the receiver, the decoder. Every frame must come back. */
static void on_phys_audio(void* c, const uint8_t* frame, const float* soft) {
    (void)c;
    sstv_rx_phys(&rx, frame, soft);
}
static void test_audio(void) {
    stream_len = encode();
    sstv_blob b;
    uint8_t* par = malloc((size_t)sstv_blob_parity_bytes(stream_len));
    sstv_blob_init(&b, stream, stream_len, true, "Bouncer", par);
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, 40);
    sstv_mod_set_band(&m, SSTV_BAND_HF);
    int cap = (int)(44100.0 * ((double)b.total * SSTV_BAND_PHYS_US(SSTV_BAND_HF) / 1e6 + 10.0));
    int16_t* pcm = calloc((size_t)cap, sizeof(int16_t));
    int n = 22050;
    int k = sstv_mod_preamble(&m, pcm + n, cap - n);
    CHECK(k > 0);
    n += k;
    uint8_t blk[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    for(int f = 0; f < b.total; f++) {
        if(f % SSTV_ILEAVE == 0) sstv_blob_block(&b, f / SSTV_ILEAVE, blk);
        k = sstv_mod_frame(&m, blk + (f % SSTV_ILEAVE) * SSTV_FRAME_BYTES, pcm + n, cap - n);
        CHECK(k > 0);
        n += k;
    }
    n += 22050;
    uint32_t x = 1;
    for(int i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        pcm[i] = (int16_t)(pcm[i] + (int)((x >> 16) % 5) - 2);
    }
    static sstv_demod d;
    anim_seen = false;
    sstv_rx_init(&rx, NULL, on_meta, NULL, NULL);
    sstv_demod_init(&d, 44100.0f, on_phys_audio, NULL);
    sstv_demod_set_band(&d, SSTV_BAND_HF);
    for(int i = 0; i < n; i += 512) sstv_demod_feed(&d, pcm + i, n - i < 512 ? n - i : 512);
    printf("  audio: %d phys frames sent, %d in, ok %d bad %d repaired %d\n", b.total, rx.frames_in, rx.frames_ok, rx.frames_bad, rx.frames_repaired);
    CHECK(anim_seen);
    sstv_anim_info in;
    int hl = sstv_anim_parse(&in, rx_byte, NULL);
    CHECK(hl > 0);
    if(hl > 0) {
        static sstv_anim_dec dd;
        uint8_t* fr = malloc((size_t)N * FB);
        sstv_anim_dec_init(&dd, &in, hl, rx.note_len, fr);
        sstv_anim_dec_run(&dd, rx_byte, NULL, 1 << 30, true, NULL);
        printf("  audio: %d/%d animation frames\n", dd.frames_done, N);
        CHECK_EQ_INT(dd.frames_done, N);
        CHECK(memcmp(fr, src, sizeof src) == 0);
        free(fr);
    }
    free(pcm);
    free(par);
}

/* Two attempts at the same animation, each losing a different set of blocks (more than the
 * outer code can repair alone); the second START is identical, so the receiver merges them. A
 * third, different animation then starts fresh. */
static void feed_pass(const uint8_t* out, int frames, int lose_from, int lose_to) {
    float soft[SSTV_FRAME_BITS];
    for(int i = 0; i < SSTV_FRAME_BITS; i++) soft[i] = 1.0f;
    uint8_t bad[SSTV_FRAME_BYTES];
    for(int f = 0; f < frames; f++) {
        if(f >= lose_from && f < lose_to) {
            for(int i = 0; i < SSTV_FRAME_BYTES; i++) bad[i] = (uint8_t)(i * 37 + f);
            sstv_rx_phys(&rx, bad, soft);
        } else {
            sstv_rx_phys(&rx, out + f * SSTV_FRAME_BYTES, soft);
        }
    }
}
static void test_merge(void) {
    stream_len = encode();
    int frames = sstv_blob_frames(stream_len);
    uint8_t* out = malloc((size_t)SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    sstv_build_anim(stream, stream_len, "Bouncer", out, NULL);
    sstv_rx_init(&rx, NULL, on_meta, NULL, NULL);
    feed_pass(out, frames, 8, 32); /* 24 frames lost: beyond 12 parity */
    CHECK(!rx.merged);
    sstv_anim_info in;
    int hl = sstv_anim_parse(&in, rx_byte, NULL);
    CHECK(hl == 0); /* this attempt lost the header block */
    static sstv_anim_dec d;
    uint8_t* fr = malloc((size_t)N * FB);
    feed_pass(out, frames, 36, 60); /* second try loses other frames */
    CHECK(rx.merged);
    {
        int wrong = 0, missing = 0;
        for(int pos = 0; pos < stream_len; pos++) {
            int v = rx_byte(NULL, pos);
            if(v < 0) missing++;
            else if(v != stream[pos]) { if(wrong < 5) fprintf(stderr, "byte %d: %d vs %d\n", pos, v, stream[pos]); wrong++; }
        }
        fprintf(stderr, "merge: wrong %d missing %d of %d, repaired %d failed %d\n", wrong, missing, stream_len, rx.frames_repaired, rx.repairs_failed);
    }
    hl = sstv_anim_parse(&in, rx_byte, NULL);
    CHECK(hl > 0);
    sstv_anim_dec_init(&d, &in, hl, rx.note_len, fr);
    sstv_anim_dec_run(&d, rx_byte, NULL, 1 << 30, true, NULL);
    for(int s = 0; s < sstv_anim_segs(N); s++) CHECK_EQ_INT(sstv_anim_seg_check(&d, s, rx_byte, NULL), 1);
    CHECK_EQ_INT(d.frames_done, N);
    CHECK(memcmp(fr, src, sizeof src) == 0);
    /* a corrupted byte fails its segment's CRC; rewind re-decodes once it is right again */
    int st, en;
    sstv_anim_seg_range(&d, 1, &st, &en);
    int at = SSTV_ILEAVE + (st + 3) / SSTV_NOTE_CHUNK;
    uint8_t keep = rx.data[at][(st + 3) % SSTV_NOTE_CHUNK];
    rx.data[at][(st + 3) % SSTV_NOTE_CHUNK] ^= 0x5A;
    CHECK_EQ_INT(sstv_anim_seg_check(&d, 1, rx_byte, NULL), -1);
    sstv_anim_dec_init(&d, &in, hl, rx.note_len, fr);
    sstv_anim_dec_run(&d, rx_byte, NULL, 1 << 30, true, NULL);
    CHECK(memcmp(fr + 16 * FB, src[16], FB * 16) != 0 || d.corrupt > 0);
    rx.data[at][(st + 3) % SSTV_NOTE_CHUNK] = keep;
    CHECK_EQ_INT(sstv_anim_seg_check(&d, 1, rx_byte, NULL), 1);
    sstv_anim_dec_rewind(&d, 1);
    sstv_anim_dec_run(&d, rx_byte, NULL, 1 << 30, true, NULL);
    CHECK(memcmp(fr, src, sizeof src) == 0);
    /* after sstv_rx_forget the same START opens fresh (nothing merged) */
    sstv_rx_forget(&rx);
    sstv_build_anim(stream, stream_len, "Bouncer", out, NULL);
    feed_pass(out, 4, 99, 99);
    CHECK(!rx.merged);
    /* a different START starts clean */
    sstv_build_anim(stream, stream_len, "Other", out, NULL);
    feed_pass(out, 4, 99, 99);
    CHECK(!rx.merged);
    free(fr);
    free(out);
}

int main(void) {
    make_frames();
    test_merge();
    test_audio();
    test_pass();
    test_roundtrip();
    test_progressive();
    test_hole();
    TEST_MAIN_END();
}
