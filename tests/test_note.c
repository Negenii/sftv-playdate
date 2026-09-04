#include "sstv_frame.h"
#include "sstv_params.h"
#include "test_util.h"
#include <string.h>

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
    CHECK((word & ~(SSTV_META_NOTE_FLAG | SSTV_META_CODED_FLAG)) == (uint16_t)total);
    CHECK(memcmp(p0 + SSTV_META_NAME_OFFSET, "Hello", 5) == 0);

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
    /* 200 chars -> 8 chunks -> 8 text frames -> data 12 -> total 12+12+4 = 28. */
    CHECK(total == SSTV_ILEAVE + SSTV_NOTE_MAX_FRAMES + SSTV_OUTER_PARITY + SSTV_ILEAVE);
}

static void test_note_empty(void) {
    static uint8_t out[SSTV_MAX_FRAMES * SSTV_FRAME_BYTES];
    int total = sstv_build_note("", "Empty", out, NULL);
    /* 0 chars -> forced 1 text block -> data 8 -> total 8+12+4 = 24. */
    CHECK(total == 8 + SSTV_OUTER_PARITY + SSTV_ILEAVE);
}

int main(void) {
    test_note_layout();
    test_note_max();
    test_note_empty();
    TEST_MAIN_END();
}
