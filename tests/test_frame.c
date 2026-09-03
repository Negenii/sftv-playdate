#include "test_util.h"
#include "sstv_frame.h"
#include <string.h>

int main(void) {
    uint8_t pixels[SSTV_SEG_BYTES];
    for(int i = 0; i < SSTV_SEG_BYTES; i++) pixels[i] = (uint8_t)(i * 37 + 11);

    uint8_t frame[SSTV_FRAME_BYTES];
    sstv_frame_pack(345, pixels, frame);
    CHECK_EQ_INT(frame[0], 1);
    CHECK_EQ_INT(frame[1], 89);
    CHECK(memcmp(frame + 2, pixels, SSTV_SEG_BYTES) == 0);

    uint16_t seg = 0;
    uint8_t out[SSTV_SEG_BYTES];
    CHECK(sstv_frame_unpack(frame, &seg, out));
    CHECK_EQ_INT(seg, 345);
    CHECK(memcmp(out, pixels, SSTV_SEG_BYTES) == 0);

    /* bit and symbol accessors: 0x01 0x59 -> bits 0000 0001 0101 1001 */
    CHECK_EQ_INT(sstv_frame_bit(frame, 7), 1);
    CHECK_EQ_INT(sstv_frame_symbol(frame, 3), 1); /* bits 6,7 = 01 */
    CHECK_EQ_INT(sstv_frame_symbol(frame, 4), 1); /* bits 8,9 = 01 */
    CHECK_EQ_INT(sstv_frame_symbol(frame, 5), 1); /* bits 10,11 = 01 */
    CHECK_EQ_INT(sstv_frame_symbol(frame, 6), 2); /* bits 12,13 = 10 */
    CHECK_EQ_INT(SSTV_SYMBOLS_PER_FRAME * SSTV_BITS_PER_SYMBOL, SSTV_FRAME_BITS);

    /* up to PARITY/2 corrupted bytes are corrected blind */
    int corrected = 0;
    uint8_t bad[SSTV_FRAME_BYTES];
    memcpy(bad, frame, sizeof bad);
    bad[5] ^= 0x10;
    bad[0] ^= 0xFF;
    bad[SSTV_FRAME_BYTES - 1] ^= 0x01;
    bad[20] ^= 0x80;
    CHECK(sstv_frame_unpack_soft(bad, NULL, &seg, out, &corrected));
    CHECK_EQ_INT(corrected, 4);
    CHECK_EQ_INT(seg, 345);
    CHECK(memcmp(out, pixels, SSTV_SEG_BYTES) == 0);

    /* PARITY bytes corrupted: blind decode fails, but with confidence marking them it works */
    memcpy(bad, frame, sizeof bad);
    float conf[SSTV_FRAME_BYTES];
    for(int i = 0; i < SSTV_FRAME_BYTES; i++) conf[i] = 1.0f;
    for(int i = 0; i < SSTV_PARITY_BYTES; i++) {
        bad[3 + i * 3] ^= (uint8_t)(0x5A + i);
        conf[3 + i * 3] = 0.05f;
    }
    CHECK(!sstv_frame_unpack_soft(bad, NULL, &seg, out, &corrected));
    CHECK(sstv_frame_unpack_soft(bad, conf, &seg, out, &corrected));
    CHECK_EQ_INT(seg, 345);
    CHECK(memcmp(out, pixels, SSTV_SEG_BYTES) == 0);

    /* interleave round trip and byte placement */
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES], physical[SSTV_ILEAVE * SSTV_FRAME_BYTES],
        back[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    for(int i = 0; i < (int)sizeof logical; i++) logical[i] = (uint8_t)(i * 7 + 3);
    sstv_interleave(logical, physical);
    CHECK_EQ_INT(physical[0], logical[0]);
    CHECK_EQ_INT(physical[1], logical[SSTV_FRAME_BYTES]);
    CHECK_EQ_INT(physical[SSTV_ILEAVE], logical[1]);
    sstv_deinterleave(physical, back);
    CHECK(memcmp(back, logical, sizeof logical) == 0);

    /* build_physical: block 0 de-interleaves to segments 0..ILEAVE-1 of the image */
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) image[i] = (uint8_t)(i ^ (i >> 5));
    uint8_t* phys = malloc(SSTV_NUM_SEGS * SSTV_FRAME_BYTES);
    sstv_build_physical(image, phys);
    sstv_deinterleave(phys, back);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        CHECK(sstv_frame_unpack(back + i * SSTV_FRAME_BYTES, &seg, out));
        CHECK_EQ_INT(seg, i);
        CHECK(memcmp(out, image + sstv_seg_line(i) * SSTV_LINE_BYTES + sstv_seg_byte_offset(i), SSTV_SEG_BYTES) == 0);
    }
    /* last block */
    sstv_deinterleave(phys + (SSTV_NUM_BLOCKS - 1) * SSTV_ILEAVE * SSTV_FRAME_BYTES, back);
    CHECK(sstv_frame_unpack(back + (SSTV_ILEAVE - 1) * SSTV_FRAME_BYTES, &seg, out));
    CHECK_EQ_INT(seg, SSTV_NUM_SEGS - 1);
    free(image);
    free(phys);
    TEST_MAIN_END();
}
