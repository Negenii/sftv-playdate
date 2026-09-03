#include "test_util.h"
#include "rs.h"
#include <string.h>

static uint32_t st = 99;
static uint32_t rnd(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}

int main(void) {
    enum { K = 28, N = K + RS_PARITY };
    (void)0; /* RS_PARITY follows SSTV_PARITY_BYTES */
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

    /* Errors and erasures: 2t + e <= RS_PARITY must decode exactly. */
    int ee_trials = 0, ee_ok = 0;
    for(int e = 0; e <= RS_PARITY; e++) {
        for(int t = 0; 2 * t + e <= RS_PARITY; t++) {
            for(int rep = 0; rep < 60; rep++) {
                for(int i = 0; i < K; i++) data[i] = (uint8_t)rnd();
                memcpy(ref, data, K);
                rs_encode(data, K, ref + K);
                memcpy(block, ref, N);
                int pos[RS_PARITY];
                int np = 0;
                while(np < e + t) {
                    int p = (int)(rnd() % N);
                    int dup = 0;
                    for(int q = 0; q < np; q++) dup |= (pos[q] == p);
                    if(!dup) pos[np++] = p;
                }
                /* erasures: corrupt (or not: an erased byte may be right) the first e */
                for(int q = 0; q < e; q++) if(rnd() & 1) block[pos[q]] ^= (uint8_t)(1 + rnd() % 255);
                for(int q = e; q < e + t; q++) block[pos[q]] ^= (uint8_t)(1 + rnd() % 255);
                int r = rs_decode_erasures(block, N, pos, e);
                ee_trials++;
                if(r >= 0 && memcmp(block, ref, N) == 0) ee_ok++;
            }
        }
    }
    printf("  rs: errors+erasures %d/%d exact\n", ee_ok, ee_trials);
    CHECK_EQ_INT(ee_ok, ee_trials);

    /* Too many errors: must not claim success silently (returns -1) most of the time. */
    int flagged = 0, silent_wrong = 0;
    for(int t = 0; t < 300; t++) {
        for(int i = 0; i < K; i++) data[i] = (uint8_t)rnd();
        memcpy(ref, data, K);
        rs_encode(data, K, ref + K);
        memcpy(block, ref, N);
        for(int e = 0; e < RS_PARITY + 2; e++) block[rnd() % N] ^= (uint8_t)(1 + rnd() % 255);
        int r = rs_decode(block, N);
        if(r < 0) flagged++;
        else if(memcmp(block, ref, N) != 0) silent_wrong++;
    }
    printf("  rs: %d errors -> flagged %d/300, miscorrected %d/300\n", RS_PARITY + 2, flagged, silent_wrong);
    CHECK(flagged >= 250); /* miscorrections are caught by the CRC one layer up */
    /* streaming encoder matches rs_encode, including short (shortened codeword) inputs */
    for(int len = 1; len <= 200; len += 37) {
        uint8_t d[200], p1[RS_PARITY], p2[RS_PARITY];
        for(int i = 0; i < len; i++) d[i] = (uint8_t)rnd();
        rs_encode(d, len, p1);
        rs_encoder_t e;
        rs_encoder_init(&e);
        for(int i = 0; i < len; i++) rs_encoder_feed(&e, d[i]);
        rs_encoder_finish(&e, p2);
        CHECK(memcmp(p1, p2, RS_PARITY) == 0);
    }
    /* a shortened codeword (n = 20) with RS_PARITY erasures decodes */
    {
        uint8_t blk[20], ref[20];
        for(int i = 0; i < 8; i++) blk[i] = (uint8_t)rnd();
        rs_encode(blk, 8, blk + 8);
        memcpy(ref, blk, 20);
        int pos[RS_PARITY];
        for(int i = 0; i < RS_PARITY; i++) {
            pos[i] = i + 3;
            blk[i + 3] = 0;
        }
        CHECK(rs_decode_erasures(blk, 20, pos, RS_PARITY) >= 0);
        CHECK(memcmp(blk, ref, 20) == 0);
    }
    TEST_MAIN_END();
}
