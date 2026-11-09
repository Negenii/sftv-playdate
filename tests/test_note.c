#include "sstv_frame.h"
#include "sstv_params.h"
#include "sstv_rx.h"
#include "test_util.h"
#include <string.h>
#include <stdlib.h>

/* Deinterleave one physical block and return the header of its logical frame i. */
static uint16_t frame_hdr(const uint8_t* phys_block, int i) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(phys_block, logical);
    uint16_t hdr;
    uint8_t payload[SSTV_PAYLOAD_BYTES];
    sstv_frame_unpack(logical + i * SSTV_FRAME_BYTES, &hdr, payload);
    return hdr;
}

static void frame_payload(const uint8_t* phys_block, int i, uint8_t* payload) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(phys_block, logical);
    uint16_t hdr;
    sstv_frame_unpack(logical + i * SSTV_FRAME_BYTES, &hdr, payload);
}

static const uint8_t* block(const uint8_t* out, int b) {
    return out + b * SSTV_ILEAVE * SSTV_FRAME_BYTES;
}

static void test_note_layout(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    const char* text = "Hello from Flipper! This is a note."; /* 35 chars -> 2 chunks -> 4 text frames */
    int total = sstv_build_note(text, "Hello", out, NULL);

    /* data = 4 START + 4 text = 8; total = 8 + 12 parity + 4 END = 24. */
    CHECK(total % SSTV_ILEAVE == 0);
    CHECK(total == 8 + SSTV_OUTER_PARITY + SSTV_ILEAVE);

    /* Block 0: START meta with the note flag set, name preserved. */
    uint16_t h0 = frame_hdr(block(out, 0), 0);
    CHECK(sstv_hdr_type(h0) == SSTV_FT_META);
    uint8_t p0[SSTV_PAYLOAD_BYTES];
    frame_payload(block(out, 0), 0, p0);
    CHECK(p0[0] == SSTV_META_START);
    uint16_t word = (uint16_t)(p0[1] | (p0[2] << 8));
    CHECK((word & SSTV_META_NOTE_FLAG) != 0);
    CHECK((word & SSTV_META_CODED_FLAG) == 0);
    CHECK((word & SSTV_META_UTF8_FLAG) == 0); /* ASCII note */
    CHECK((word & ~(SSTV_META_NOTE_FLAG | SSTV_META_CODED_FLAG | SSTV_META_UTF8_FLAG)) == (uint16_t)total);
    int len = p0[SSTV_NOTE_LEN_OFFSET] | (p0[SSTV_NOTE_LEN_OFFSET + 1] << 8);
    CHECK(len == (int)strlen(text));
    CHECK(memcmp(p0 + SSTV_NOTE_NAME_OFFSET, "Hello", 5) == 0);

    /* Block 1: RAW text frames, index = chunk, first bytes are the text. */
    uint16_t ht = frame_hdr(block(out, 1), 0);
    CHECK(sstv_hdr_type(ht) == SSTV_FT_RAW);
    CHECK(sstv_hdr_index(ht) == 0);
    uint8_t pt[SSTV_PAYLOAD_BYTES];
    frame_payload(block(out, 1), 0, pt);
    CHECK(memcmp(pt, text, SSTV_NOTE_CHUNK) == 0);

    /* Last block: END meta with the note flag. */
    uint8_t pe[SSTV_PAYLOAD_BYTES];
    frame_payload(block(out, total / SSTV_ILEAVE - 1), 0, pe);
    CHECK(pe[0] == SSTV_META_END);
}

static void test_note_max(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    char text[SSTV_NOTE_MAX_CHARS + 1];
    for(int i = 0; i < SSTV_NOTE_MAX_CHARS; i++) text[i] = (char)('A' + i % 26);
    text[SSTV_NOTE_MAX_CHARS] = 0;
    int total = sstv_build_note(text, "Max", out, NULL);
    int d = SSTV_ILEAVE + SSTV_NOTE_MAX_FRAMES;
    CHECK_EQ_INT(total, sstv_pass_total(d, SSTV_NOTE_INTRO(d))); /* multi-group layout */
    CHECK(total <= SSTV_MAX_FRAMES); /* fits the Flipper's picture buffer */
    /* a 1000-char note keeps the single-group layout: 4 + 40 + SSTV_OUTER_PARITY + 4 */
    text[1000] = 0;
    CHECK_EQ_INT(sstv_build_note(text, "K", out, NULL), 48 + SSTV_OUTER_PARITY);
}

static void test_note_empty(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    int total = sstv_build_note("", "Empty", out, NULL);
    /* 0 chars -> forced 1 text block -> data 8 -> total 8+12+4 = 24. */
    CHECK(total == 8 + SSTV_OUTER_PARITY + SSTV_ILEAVE);
}


/* ---- receiver loopback (frame level, like test_outer) ---- */
static char note_got[SSTV_NOTE_MAX_BYTES + 64];
static int note_starts, note_ends;

static void n_pixels(void* c, uint16_t l, int xb, int nb, const uint8_t* p) { (void)c;(void)l;(void)xb;(void)nb;(void)p; }
static void n_preview(void* c, int a, int b, int nb, const uint8_t* d) { (void)c;(void)a;(void)b;(void)nb;(void)d; }
static void n_meta(void* c, const char* nm, bool end, int t, bool cd) { (void)c;(void)nm;(void)t;(void)cd; if(end) note_ends++; else note_starts++; }
static void n_note(void* c, int chunk, int nbytes, const uint8_t* text) {
    (void)c;
    int off = chunk * SSTV_NOTE_CHUNK;
    for(int k = 0; k < nbytes && off + k < (int)sizeof note_got - 1; k++) note_got[off + k] = (char)text[k];
}

/* Feeds every physical frame; frames in `skip` (whole 4-frame blocks) never arrive. */
static void feed_note(const char* text, const char* name, const int* skip, int n_skip) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    int total = sstv_build_note(text, name, out, NULL);
    float soft[SSTV_FRAME_BITS];
    for(int i = 0; i < SSTV_FRAME_BITS; i++) soft[i] = 1.0f;
    memset(note_got, 0, sizeof note_got);
    note_starts = note_ends = 0;
    sstv_rx rx;
    sstv_rx_init(&rx, n_pixels, n_meta, n_preview, NULL);
    sstv_rx_set_note_cb(&rx, n_note);
    for(int f = 0; f < total; f++) {
        bool drop = false;
        for(int i = 0; i < n_skip; i++) if(skip[i] == f) drop = true;
        if(drop) continue;
        sstv_rx_phys(&rx, out + f * SSTV_FRAME_BYTES, soft);
    }
    /* trim trailing NULs */
    int n = (int)strlen(note_got);
    (void)n;
}

static void test_note_loopback(void) {
    feed_note("Hello from Flipper! This is a note.", "Hello", NULL, 0);
    CHECK(strcmp(note_got, "Hello from Flipper! This is a note.") == 0);
    CHECK_EQ_INT(note_starts, 1);
    CHECK_EQ_INT(note_ends, 1);

    feed_note("", "Empty", NULL, 0);
    CHECK(strcmp(note_got, "") == 0);
}

static void test_note_repair(void) {
    char text[SSTV_NOTE_MAX_CHARS + 1];
    for(int i = 0; i < SSTV_NOTE_MAX_CHARS; i++) text[i] = (char)('!' + i % 90);
    text[SSTV_NOTE_MAX_CHARS] = 0;
    /* drop the two whole text blocks (physical frames 4..11); the outer parity rebuilds them */
    int skip[8] = {4, 5, 6, 7, 8, 9, 10, 11};
    feed_note(text, "Max", skip, 8);
    CHECK(strcmp(note_got, text) == 0);
}

/* A note longer than one outer group: every group repairs its own losses. */
static void test_note_multigroup(void) {
    static char text[SSTV_NOTE_MAX_CHARS + 1];
    for(int i = 0; i < SSTV_NOTE_MAX_CHARS; i++) text[i] = (char)('a' + (i * 7) % 26);
    text[SSTV_NOTE_MAX_CHARS] = 0;
    feed_note(text, "Long", NULL, 0);
    CHECK(strcmp(note_got, text) == 0);
    /* lose a whole block in group 0 and one in group 2 (data frames 4..7 of each) */
    int d = SSTV_ILEAVE + SSTV_NOTE_MAX_FRAMES, intro = SSTV_NOTE_INTRO(d);
    int g2 = sstv_pass_group_base(intro, 2), at = 0; /* physical frame of data position g2 */
    for(int p = 0; p < 4000 && sstv_pass_data_before(p, d, intro) < g2; p++) at = p + 1;
    int skip[8] = {8, 9, 10, 11, at + 4, at + 5, at + 6, at + 7};
    feed_note(text, "Long", skip, 8);
    CHECK(strcmp(note_got, text) == 0);
}

static void test_note_variable(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    /* 10 chars -> 1 chunk -> 1 block (4 text frames) -> total 24 */
    CHECK(sstv_build_note("0123456789", "S", out, NULL) == 8 + SSTV_OUTER_PARITY + SSTV_ILEAVE);
    /* 130 chars -> 6 chunks -> 8 text frames -> total 28 (more content, more frames) */
    char t[131];
    for(int i = 0; i < 130; i++) t[i] = 'x';
    t[130] = 0;
    CHECK(sstv_build_note(t, "M", out, NULL) == 12 + SSTV_OUTER_PARITY + SSTV_ILEAVE);
}

static void test_note_utf8(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    const char* text = "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"; /* "Привет" in UTF-8 */
    sstv_build_note(text, "hi", out, NULL);
    uint8_t p0[SSTV_PAYLOAD_BYTES];
    frame_payload(block(out, 0), 0, p0);
    uint16_t word = (uint16_t)(p0[1] | (p0[2] << 8));
    CHECK((word & SSTV_META_UTF8_FLAG) != 0); /* flagged as UTF-8 */
    int len = p0[SSTV_NOTE_LEN_OFFSET] | (p0[SSTV_NOTE_LEN_OFFSET + 1] << 8);
    CHECK(len == 12);
    feed_note(text, "hi", NULL, 0);
    CHECK(strcmp(note_got, text) == 0); /* bytes round-trip */
}

int main(void) {
    test_note_layout();
    test_note_max();
    test_note_empty();
    test_note_variable();
    test_note_utf8();
    test_note_loopback();
    test_note_repair();
    test_note_multigroup();
    TEST_MAIN_END();
}
