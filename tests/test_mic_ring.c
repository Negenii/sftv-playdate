#include "test_util.h"
#include "mic_ring.h"

int main(void) {
    static mic_ring r;
    mic_ring_init(&r);
    int16_t in[100], out[100];
    for(int i = 0; i < 100; i++) in[i] = (int16_t)i;

    CHECK_EQ_INT(mic_ring_read(&r, out, 100), 0);
    CHECK_EQ_INT(mic_ring_write(&r, in, 100), 100);
    CHECK_EQ_INT(mic_ring_read(&r, out, 40), 40);
    CHECK_EQ_INT(out[39], 39);
    CHECK_EQ_INT(mic_ring_read(&r, out, 100), 60);
    CHECK_EQ_INT(out[0], 40);
    CHECK_EQ_INT(out[59], 99);

    /* wrap around: fill to near capacity many times */
    int total_in = 0, total_out = 0;
    for(int round = 0; round < 1000; round++) {
        total_in += mic_ring_write(&r, in, 100);
        total_out += mic_ring_read(&r, out, 100);
        CHECK_EQ_INT(out[0], 0);
        CHECK_EQ_INT(out[99], 99);
    }
    CHECK_EQ_INT(total_in, total_out);

    /* full: extra samples are dropped, not overwritten */
    mic_ring_init(&r);
    static int16_t big[MIC_RING_CAP + 10];
    for(int i = 0; i < MIC_RING_CAP + 10; i++) big[i] = (int16_t)(i & 0x7FFF);
    CHECK_EQ_INT(mic_ring_write(&r, big, MIC_RING_CAP + 10), MIC_RING_CAP - 1);
    CHECK_EQ_INT(mic_ring_write(&r, big, 10), 0);
    CHECK_EQ_INT(mic_ring_read(&r, out, 1), 1);
    CHECK_EQ_INT(out[0], 0);
    TEST_MAIN_END();
}
