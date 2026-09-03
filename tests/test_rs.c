#include "test_util.h"
#include "rs.h"
#include <string.h>

static uint32_t st = 99;
static uint32_t rnd(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}

int main(void) {
    enum { K = 52, N = K + RS_PARITY };
    uint8_t data[K], block[N], ref[N];

    /* Clean block decodes with 0 corrections and no change. */
    for(int i = 0; i < K; i++) data[i] = (uint8_t)rnd();
    memcpy(block, data, K);
    rs_encode(data, K, block + K);
    memcpy(ref, block, N);
    CHECK_EQ_INT(rs_decode(block, N), 0);
    CHECK(memcmp(block, ref, N) == 0);

    /* Systematic: data bytes unchanged by encoding. */
    CHECK(memcmp(block, data, K) == 0);

    /* 1..6 random symbol errors (data or parity) are corrected exactly. */
    int trials = 0, ok = 0;
    for(int errs = 1; errs <= RS_MAX_ERRORS; errs++) {
        for(int t = 0; t < 200; t++) {
            for(int i = 0; i < K; i++) data[i] = (uint8_t)rnd();
            memcpy(ref, data, K);
            rs_encode(data, K, ref + K);
            memcpy(block, ref, N);
            /* distinct positions */
            int pos[RS_MAX_ERRORS];
            for(int e = 0; e < errs; e++) {
                int p;
                int dup;
                do {
                    p = (int)(rnd() % N);
                    dup = 0;
                    for(int q = 0; q < e; q++) dup |= (pos[q] == p);
                } while(dup);
                pos[e] = p;
                block[p] ^= (uint8_t)(1 + rnd() % 255);
            }
            int r = rs_decode(block, N);
            trials++;
            if(r == errs && memcmp(block, ref, N) == 0) ok++;
        }
    }
    printf("  rs: %d/%d blocks with 1..%d errors corrected exactly\n", ok, trials, RS_MAX_ERRORS);
    CHECK_EQ_INT(ok, trials);

    /* Too many errors: must not claim success silently (returns -1) most of the time. */
    int flagged = 0, silent_wrong = 0;
    for(int t = 0; t < 300; t++) {
        for(int i = 0; i < K; i++) data[i] = (uint8_t)rnd();
        memcpy(ref, data, K);
        rs_encode(data, K, ref + K);
        memcpy(block, ref, N);
        for(int e = 0; e < 9; e++) block[rnd() % N] ^= (uint8_t)(1 + rnd() % 255);
        int r = rs_decode(block, N);
        if(r < 0) flagged++;
        else if(memcmp(block, ref, N) != 0) silent_wrong++;
    }
    printf("  rs: 9 errors -> flagged %d/300, miscorrected %d/300\n", flagged, silent_wrong);
    CHECK(flagged >= 250); /* miscorrections are caught by the CRC one layer up */
    TEST_MAIN_END();
}
