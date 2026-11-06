#include "test_util.h"
#include "sstv_frame.h"
#include "rs.h"
#include "sstv_preview.h"
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
    CHECK(frames <= sstv_pass_total(88 + SSTV_INTRO_FRAMES_CODED, sstv_pass_intro(1)));
    CHECK_EQ_INT(lines_after[frames - 1], SSTV_HEIGHT);
    /* meta block first: name in every logical frame */
    sstv_deinterleave(phys, back);
    uint8_t pl[SSTV_BLOCK_PAYLOAD];
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl + i * SSTV_PAYLOAD_BYTES));
        CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
        CHECK_EQ_INT(pl[i * SSTV_PAYLOAD_BYTES], SSTV_META_START);
        int word = pl[i * SSTV_PAYLOAD_BYTES + 1] | (pl[i * SSTV_PAYLOAD_BYTES + 2] << 8);
        CHECK_EQ_INT(word, frames | SSTV_META_CODED_FLAG); /* announces the pass length and mode */
        CHECK(memcmp(pl + i * SSTV_PAYLOAD_BYTES + SSTV_META_NAME_OFFSET, "white.pbm", 9) == 0);
    }
    /* schedule block after the previews: lines per coded block, summing to the height */
    {
        sstv_deinterleave(phys + (SSTV_ILEAVE + SSTV_INTRO_FRAMES) * SSTV_FRAME_BYTES, back);
        int sum = 0, nblocks = 0;
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl));
            CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
            CHECK_EQ_INT(pl[0], SSTV_META_SCHEDULE);
            for(int k = 1; k <= SSTV_PREVIEW_CHUNK; k++) {
                sum += pl[k];
                if(pl[k]) nblocks++;
            }
        }
        CHECK_EQ_INT(sum, SSTV_HEIGHT);
        int coded_data = SSTV_ILEAVE + SSTV_INTRO_FRAMES_CODED + nblocks * SSTV_ILEAVE;
        CHECK_EQ_INT(frames, sstv_pass_total(coded_data, sstv_pass_intro(1)));
    }
    /* preview: a white image gives an all-zero preview, chunks 0..19 follow the start block */
    {
        uint8_t pv[SSTV_PREVIEW_BYTES];
        sstv_preview_make(image, pv);
        for(int i = 0; i < SSTV_PREVIEW_BYTES; i++) CHECK_EQ_INT(pv[i], 0);
        memset(image, 0xFF, SSTV_IMAGE_BYTES);
        sstv_preview_make(image, pv);
        for(int i = 0; i < SSTV_PREVIEW_BYTES; i++) CHECK_EQ_INT(pv[i], 0xFF);
        /* 50 % checker (1x1) -> 13/25 black -> half the preview pixels set */
        for(int y = 0; y < SSTV_HEIGHT; y++) for(int i = 0; i < SSTV_LINE_BYTES; i++) image[y * SSTV_LINE_BYTES + i] = (y & 1) ? 0x55 : 0xAA;
        sstv_preview_make(image, pv);
        int set = 0;
        for(int i = 0; i < SSTV_PREVIEW_BYTES; i++) for(int b = 0; b < 8; b++) set += (pv[i] >> b) & 1;
        CHECK(set > SSTV_PREVIEW_BYTES * 8 * 4 / 10 && set < SSTV_PREVIEW_BYTES * 8 * 6 / 10);
        memset(image, 0, SSTV_IMAGE_BYTES);
    }
    /* a random image is sent raw: start, tiny, 5 preview blocks, 480 segment frames, end */
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) image[i] = (uint8_t)((i * 2654435761u) >> 13);
    frames = sstv_build_physical(image, "noise", phys, lines_after, &coded);
    CHECK(!coded);
    const int raw_data = SSTV_ILEAVE + SSTV_INTRO_FRAMES + SSTV_NUM_SEGS; /* 508 */
    const int raw_intro = sstv_pass_intro(0);
    CHECK_EQ_INT(frames, sstv_pass_total(raw_data, raw_intro));
    /* the intro is group 0: its 12 parity frames come right after it (physical frame raw_intro)
       and carry the RS parity of every payload column of positions 0..raw_intro-1 */
    {
        uint8_t col[SSTV_OUTER_GROUP + SSTV_OUTER_PARITY];
        uint8_t got[SSTV_OUTER_PARITY][SSTV_PAYLOAD_BYTES];
        for(int p = 0; p < SSTV_OUTER_PARITY; p += SSTV_ILEAVE) {
            sstv_deinterleave(phys + (raw_intro + p) * SSTV_FRAME_BYTES, back);
            for(int i = 0; i < SSTV_ILEAVE; i++) {
                CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, got[p + i]));
                CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_PARITY);
                CHECK_EQ_INT(sstv_hdr_index(blk), p + i);
            }
        }
        for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) {
            for(int pos = 0; pos < raw_intro; pos++) {
                sstv_deinterleave(phys + (pos / SSTV_ILEAVE) * SSTV_ILEAVE * SSTV_FRAME_BYTES, back);
                col[pos] = back[(pos % SSTV_ILEAVE) * SSTV_FRAME_BYTES + 2 + k];
            }
            uint8_t par[SSTV_OUTER_PARITY];
            rs_encode(col, raw_intro, par);
            for(int j = 0; j < SSTV_OUTER_PARITY; j++) CHECK_EQ_INT(got[j][k], par[j]);
        }
    }
    /* END is the last block and announces the same total */
    sstv_deinterleave(phys + (frames - SSTV_ILEAVE) * SSTV_FRAME_BYTES, back);
    CHECK(sstv_frame_unpack(back, &blk, pl));
    CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
    CHECK_EQ_INT(pl[0], SSTV_META_END);
    CHECK_EQ_INT(pl[1] | (pl[2] << 8), frames);
    /* the frame kinds agree with the layout helper for every physical frame */
    for(int f = 0; f < frames; f++) {
        sstv_deinterleave(phys + (f / SSTV_ILEAVE) * SSTV_ILEAVE * SSTV_FRAME_BYTES, back);
        CHECK(sstv_frame_unpack(back + (f % SSTV_ILEAVE) * SSTV_FRAME_BYTES, &blk, pl));
        int kind = sstv_pass_frame_kind(f, raw_data, raw_intro);
        int type = sstv_hdr_type(blk);
        if(kind == SSTV_PASS_PARITY) CHECK_EQ_INT(type, SSTV_FT_PARITY);
        else if(kind == SSTV_PASS_END) CHECK(type == SSTV_FT_META && pl[0] == SSTV_META_END);
        else CHECK(type != SSTV_FT_PARITY);
        /* data frames sit at the position the helpers predict */
        if(kind == SSTV_PASS_DATA) {
            int pos = type == SSTV_FT_META ? sstv_pass_meta_position(pl[0], sstv_hdr_index(blk), f % SSTV_ILEAVE) : sstv_pass_position(blk, f % SSTV_ILEAVE);
            CHECK_EQ_INT(pos, sstv_pass_data_before(f, raw_data, raw_intro));
        }
    }
    CHECK_EQ_INT(lines_after[frames - 1], SSTV_HEIGHT);
    const int data0 = SSTV_ILEAVE + SSTV_INTRO_FRAMES + SSTV_OUTER_PARITY; /* first picture frame: after START, previews and the intro parity */
    CHECK_EQ_INT(lines_after[data0 - 1], 0);
    CHECK_EQ_INT(lines_after[data0 + 3], 2); /* first data block = lines 0,1 */
    {
        uint8_t tn[SSTV_TINY_BYTES];
        sstv_preview_make_tiny(image, tn);
        sstv_deinterleave(phys + SSTV_ILEAVE * SSTV_FRAME_BYTES, back); /* tiny block */
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl));
            CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
            CHECK_EQ_INT(sstv_hdr_index(blk), i);
            CHECK_EQ_INT(pl[0], SSTV_META_TINY);
            CHECK(memcmp(pl + 1, tn + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK) == 0);
        }
        uint8_t pv[SSTV_PREVIEW_BYTES];
        sstv_preview_make(image, pv);
        sstv_deinterleave(phys + (SSTV_ILEAVE + SSTV_TINY_CHUNKS) * SSTV_FRAME_BYTES, back); /* first preview block */
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl));
            CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_META);
            CHECK_EQ_INT(sstv_hdr_index(blk), i);
            CHECK_EQ_INT(pl[0], SSTV_META_PREVIEW);
            CHECK(memcmp(pl + 1, pv + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK) == 0);
        }
    }
    sstv_deinterleave(phys + data0 * SSTV_FRAME_BYTES, back);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &blk, pl));
        CHECK_EQ_INT(sstv_hdr_type(blk), SSTV_FT_RAW);
        CHECK_EQ_INT(sstv_hdr_index(blk), i);
        CHECK(memcmp(pl, image + sstv_seg_line(i) * SSTV_LINE_BYTES + sstv_seg_byte_offset(i), SSTV_SEG_BYTES) == 0);
    }
    /* pass layout helpers (revision 7) */
    CHECK_EQ_INT(sstv_pass_position(sstv_hdr(SSTV_FT_META, 0), 2), -1); /* META: use the meta variant */
    CHECK_EQ_INT(sstv_pass_position(sstv_hdr(SSTV_FT_RAW, 5), 1), 28 + 5); /* raw segment */
    CHECK_EQ_INT(sstv_pass_position(sstv_hdr(SSTV_FT_CODED, 7), 3), 32 + 3); /* first coded block */
    CHECK_EQ_INT(sstv_pass_position(sstv_hdr(SSTV_FT_PARITY, 5), 0), -1);
    CHECK_EQ_INT(sstv_pass_meta_position(SSTV_META_START, 0, 3), 3);
    CHECK_EQ_INT(sstv_pass_meta_position(SSTV_META_TINY, 2, 2), 4 + 2);
    CHECK_EQ_INT(sstv_pass_meta_position(SSTV_META_PREVIEW, 19, 3), 8 + 19);
    CHECK_EQ_INT(sstv_pass_meta_position(SSTV_META_SCHEDULE, 1, 1), 28 + 1);
    CHECK_EQ_INT(sstv_pass_meta_position(SSTV_META_END, 1, 1), -1);
    /* grouping: intro is group 0, then SSTV_OUTER_GROUP-frame data groups */
    const int intro = sstv_pass_intro(0); /* raw intro = SSTV_ILEAVE + SSTV_INTRO_FRAMES */
    CHECK_EQ_INT(sstv_pass_intro(0), SSTV_ILEAVE + SSTV_INTRO_FRAMES);
    CHECK_EQ_INT(sstv_pass_groups(508, intro), 1 + (508 - intro + SSTV_OUTER_GROUP - 1) / SSTV_OUTER_GROUP);
    CHECK_EQ_INT(sstv_pass_group_size(508, intro, 0), intro);
    CHECK_EQ_INT(sstv_pass_group_size(508, intro, 1), SSTV_OUTER_GROUP);
    CHECK_EQ_INT(sstv_pass_group_base(intro, 0), 0);
    CHECK_EQ_INT(sstv_pass_group_base(intro, 1), intro);
    CHECK_EQ_INT(sstv_pass_group_base(intro, 2), intro + SSTV_OUTER_GROUP);
    CHECK_EQ_INT(sstv_pass_group_of(0, intro), 0);
    CHECK_EQ_INT(sstv_pass_group_of(intro - 1, intro), 0);
    CHECK_EQ_INT(sstv_pass_group_of(intro, intro), 1);
    CHECK_EQ_INT(sstv_pass_group_of(intro + SSTV_OUTER_GROUP, intro), 2);
    CHECK_EQ_INT(sstv_pass_total(508, intro), 508 + sstv_pass_groups(508, intro) * SSTV_OUTER_PARITY + SSTV_ILEAVE);
    /* frame_kind: intro data, then intro parity, then group-1 data */
    CHECK_EQ_INT(sstv_pass_frame_kind(intro - 1, 508, intro), SSTV_PASS_DATA);
    CHECK_EQ_INT(sstv_pass_frame_kind(intro, 508, intro), SSTV_PASS_PARITY);
    CHECK_EQ_INT(sstv_pass_frame_kind(intro + SSTV_OUTER_PARITY, 508, intro), SSTV_PASS_DATA);
    CHECK_EQ_INT(sstv_pass_frame_kind(sstv_pass_total(508, intro) - 1, 508, intro), SSTV_PASS_END);
    CHECK_EQ_INT(sstv_pass_data_before(0, 508, intro), 0);
    CHECK_EQ_INT(sstv_pass_data_before(intro, 508, intro), intro);
    CHECK_EQ_INT(sstv_pass_data_before(intro + 3, 508, intro), intro); /* inside intro parity */
    CHECK_EQ_INT(sstv_pass_data_before(intro + SSTV_OUTER_PARITY + 1, 508, intro), intro + 1);
    CHECK_EQ_INT(sstv_pass_data_before(sstv_pass_total(508, intro), 508, intro), 508);
    CHECK(SSTV_MAX_FRAMES >= sstv_pass_total(SSTV_MAX_DATA_FRAMES, sstv_pass_intro(1)));

    /* Lock frames (start preamble) must never decode: alone, soft, or deinterleaved from the
     * SSTV_ILEAVE identical copies a receiver gets. */
    {
        uint8_t lock[SSTV_FRAME_BYTES], phys4[SSTV_ILEAVE * SSTV_FRAME_BYTES], logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
        uint8_t pl[SSTV_PAYLOAD_BYTES];
        uint16_t h;
        int corr;
        sstv_lock_frame(lock);
        CHECK(!sstv_frame_unpack(lock, &h, pl));
        CHECK(!sstv_frame_unpack_soft(lock, NULL, &h, pl, &corr));
        for(int i = 0; i < SSTV_ILEAVE; i++) memcpy(phys4 + i * SSTV_FRAME_BYTES, lock, SSTV_FRAME_BYTES);
        sstv_deinterleave(phys4, logical);
        for(int i = 0; i < SSTV_ILEAVE; i++) CHECK(!sstv_frame_unpack(logical + i * SSTV_FRAME_BYTES, &h, pl));
        /* ladder: tone 1, 2, 3 blocks with silence gaps */
        CHECK_EQ_INT(sstv_leader_symbol(0), 1);
        CHECK_EQ_INT(sstv_leader_symbol(SSTV_LADDER_TONE_SYMBOLS), 0);
        CHECK_EQ_INT(sstv_leader_symbol(SSTV_LADDER_TONE_SYMBOLS + SSTV_LADDER_GAP_SYMBOLS), 2);
        CHECK_EQ_INT(sstv_leader_symbol(SSTV_LEADER_SYMBOLS - 1), 0);
        CHECK_EQ_INT(sstv_leader_symbol(SSTV_LEADER_SYMBOLS - SSTV_LADDER_GAP_SYMBOLS - 1), 3);
    }

    free(lines_after);
    free(image);
    free(phys);
    TEST_MAIN_END();
}
