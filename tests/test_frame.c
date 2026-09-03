#include "test_util.h"
#include "sstv_frame.h"
#include <string.h>

typedef struct {
    int n;
    uint16_t lines[SSTV_HEIGHT];
    uint8_t px[SSTV_HEIGHT][SSTV_LINE_BYTES];
} cap;
static void on_line(void* ctx, uint16_t line, const uint8_t* pixels) {
    cap* c = ctx;
    c->lines[c->n] = line;
    memcpy(c->px[c->n], pixels, SSTV_LINE_BYTES);
    c->n++;
}

int main(void) {
    uint8_t payload[SSTV_PAYLOAD_BYTES];
    for(int i = 0; i < SSTV_PAYLOAD_BYTES; i++) payload[i] = (uint8_t)(i * 37 + 11);

    uint8_t frame[SSTV_FRAME_BYTES];
    sstv_frame_pack(sstv_hdr(SSTV_FT_CODED, 77), payload, frame);
    CHECK_EQ_INT(frame[0], 0x40);
    CHECK_EQ_INT(frame[1], 77);
    CHECK(memcmp(frame + 2, payload, SSTV_PAYLOAD_BYTES) == 0);

    uint16_t blk = 0;
    uint8_t out[SSTV_PAYLOAD_BYTES];
    CHECK(sstv_frame_unpack(frame, &blk, out));
    CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_CODED);
    CHECK_EQ_INT(sstv_hdr_index(blk), 77);
    CHECK(memcmp(out, payload, SSTV_PAYLOAD_BYTES) == 0);
    CHECK_EQ_INT(sstv_frame_symbol(frame, 7), 1); /* bits 14,15 of 0x40 0x4D */
    CHECK_EQ_INT(SSTV_SYMBOLS_PER_FRAME * SSTV_BITS_PER_SYMBOL, SSTV_FRAME_BITS);

    /* up to PARITY/2 corrupted bytes are corrected blind */
    int corrected = 0;
    uint8_t bad[SSTV_FRAME_BYTES];
    memcpy(bad, frame, sizeof bad);
    for(int i = 0; i < SSTV_PARITY_BYTES / 2; i++) bad[2 + i * 5] ^= (uint8_t)(0x11 + i);
    CHECK(sstv_frame_unpack_soft(bad, NULL, &blk, out, &corrected));
    CHECK_EQ_INT(corrected, SSTV_PARITY_BYTES / 2);
    CHECK(memcmp(out, payload, SSTV_PAYLOAD_BYTES) == 0);

    /* PARITY bytes corrupted: blind fails, erasures from confidence succeed */
    memcpy(bad, frame, sizeof bad);
    float conf[SSTV_FRAME_BYTES];
    for(int i = 0; i < SSTV_FRAME_BYTES; i++) conf[i] = 1.0f;
    for(int i = 0; i < SSTV_PARITY_BYTES; i++) {
        bad[1 + i * 3] ^= (uint8_t)(0x5A + i);
        conf[1 + i * 3] = 0.05f;
    }
    CHECK(!sstv_frame_unpack_soft(bad, NULL, &blk, out, &corrected));
    CHECK(sstv_frame_unpack_soft(bad, conf, &blk, out, &corrected));
    CHECK_EQ_INT(sstv_hdr_index(blk), 77);

    /* interleave round trip */
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES], physical[SSTV_ILEAVE * SSTV_FRAME_BYTES],
        back[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    for(int i = 0; i < (int)sizeof logical; i++) logical[i] = (uint8_t)(i * 7 + 3);
    sstv_interleave(logical, physical);
    CHECK_EQ_INT(physical[1], logical[SSTV_FRAME_BYTES]);
    sstv_deinterleave(physical, back);
    CHECK(memcmp(back, logical, sizeof logical) == 0);

    /* block pack/unpack on a random (incompressible) image: 2 lines per block */
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) image[i] = (uint8_t)((i * 2654435761u) >> 13);
    uint8_t blockp[SSTV_BLOCK_PAYLOAD];
    int n = sstv_block_pack(image, 10, blockp);
    CHECK_EQ_INT(n, 2); /* 2 + 2 x 51 = 104 */
    cap c = {0};
    CHECK_EQ_INT(sstv_block_unpack(blockp, SSTV_BLOCK_PAYLOAD, on_line, &c), 2);
    CHECK_EQ_INT(c.lines[0], 10);
    CHECK_EQ_INT(c.lines[1], 11);
    CHECK(memcmp(c.px[0], image + 10 * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0);
    CHECK(memcmp(c.px[1], image + 11 * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0);

    /* a white image packs many lines per block and survives truncation to the valid prefix */
    memset(image, 0, SSTV_IMAGE_BYTES);
    n = sstv_block_pack(image, 0, blockp);
    CHECK(n >= 15);
    c.n = 0;
    CHECK_EQ_INT(sstv_block_unpack(blockp, SSTV_BLOCK_PAYLOAD, on_line, &c), n);
    c.n = 0;
    int partial = sstv_block_unpack(blockp, 20, on_line, &c); /* only the first frame valid */
    CHECK(partial > 0 && partial < n);

    /* full build: a white image is coded and needs far fewer than 480 frames */
    uint8_t* phys = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    uint8_t* lines_after = malloc(SSTV_MAX_FRAMES);
    bool coded = false;
    int frames = sstv_build_physical(image, "white.pbm", phys, lines_after, &coded);
    CHECK(coded);
    CHECK_EQ_INT(frames % SSTV_ILEAVE, 0);
    CHECK(frames <= 84);
    CHECK_EQ_INT(lines_after[frames - 1], SSTV_HEIGHT);
    /* meta block first: name in every logical frame */
    sstv_deinterleave(phys, back);
    uint8_t pl[SSTV_BLOCK_PAYLOAD];
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl + i * SSTV_PAYLOAD_BYTES));
        CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
        CHECK(memcmp(pl + i * SSTV_PAYLOAD_BYTES, "white.pbm", 9) == 0);
    }
    /* a random image is sent raw: 480 segment frames + the meta block */
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) image[i] = (uint8_t)((i * 2654435761u) >> 13);
    frames = sstv_build_physical(image, "noise", phys, lines_after, &coded);
    CHECK(!coded);
    CHECK_EQ_INT(frames, SSTV_NUM_SEGS + SSTV_ILEAVE);
    CHECK_EQ_INT(lines_after[frames - 1], SSTV_HEIGHT);
    CHECK_EQ_INT(lines_after[SSTV_ILEAVE + 3], 2); /* first data block = lines 0,1 */
    sstv_deinterleave(phys + SSTV_ILEAVE * SSTV_FRAME_BYTES, back);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl));
        CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_RAW);
        CHECK_EQ_INT(sstv_hdr_index(blk), i);
        CHECK(memcmp(pl, image + sstv_seg_line(i) * SSTV_LINE_BYTES + sstv_seg_byte_offset(i), SSTV_SEG_BYTES) == 0);
    }
    free(lines_after);
    free(image);
    free(phys);
    TEST_MAIN_END();
}
