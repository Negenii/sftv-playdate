#include "test_util.h"
#include "sstv_frame.h"
#include <string.h>

int main(void) {
    uint8_t pixels[SSTV_LINE_BYTES];
    for(int i = 0; i < SSTV_LINE_BYTES; i++) pixels[i] = (uint8_t)(i * 37 + 11);

    uint8_t frame[SSTV_FRAME_BYTES];
    sstv_frame_pack(123, pixels, frame);
    CHECK_EQ_INT(frame[0], 123);
    CHECK(memcmp(frame + 1, pixels, SSTV_LINE_BYTES) == 0);

    uint8_t line_no = 0, out[SSTV_LINE_BYTES];
    CHECK(sstv_frame_unpack(frame, &line_no, out));
    CHECK_EQ_INT(line_no, 123);
    CHECK(memcmp(out, pixels, SSTV_LINE_BYTES) == 0);

    /* bit accessor: MSB first. 123 = 0b01111011 */
    CHECK_EQ_INT(sstv_frame_bit(frame, 0), 0);
    CHECK_EQ_INT(sstv_frame_bit(frame, 1), 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, 7), 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, 8), (pixels[0] >> 7) & 1);
    CHECK_EQ_INT(sstv_frame_bit(frame, SSTV_FRAME_BITS - 1), frame[SSTV_FRAME_BYTES - 1] & 1);
    /* symbol 0 = first two bits of 123 = 0b01 */
    CHECK_EQ_INT(sstv_frame_symbol(frame, 0), 1);
    CHECK_EQ_INT(sstv_frame_symbol(frame, 1), 3);

    /* up to 6 corrupted bytes are corrected by Reed-Solomon */
    int corrected = 0;
    frame[20] ^= 0x10;
    frame[0] ^= 0xFF; /* even the line number */
    frame[60] ^= 0x01; /* and parity */
    CHECK(sstv_frame_unpack_ex(frame, &line_no, out, &corrected));
    CHECK_EQ_INT(corrected, 3);
    CHECK_EQ_INT(line_no, 123);
    CHECK(memcmp(out, pixels, SSTV_LINE_BYTES) == 0);
    /* 7 corrupted bytes: RS gives up or CRC catches it */
    for(int i = 0; i < 7; i++) frame[5 + i * 7] ^= 0xA5;
    CHECK(!sstv_frame_unpack_ex(frame, &line_no, out, &corrected));

    CHECK_EQ_INT(SSTV_FRAME_BITS, 8 * SSTV_FRAME_BYTES);
    CHECK_EQ_INT(SSTV_IMAGE_BYTES, SSTV_LINE_BYTES * SSTV_HEIGHT);
    TEST_MAIN_END();
}
