#include "test_util.h"
#include "sstv_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int roundtrip_image(const char* path, int* total_out) {
    FILE* f = fopen(path, "rb");
    if(!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)size);
    if(fread(buf, 1, (size_t)size, f) != (size_t)size) return -1;
    fclose(f);
    const uint8_t* img = buf + size - SSTV_IMAGE_BYTES;
    int total = 0, bad = 0;
    uint8_t prev[SSTV_LINE_BYTES], rec[SSTV_CODEC_MAX_RECORD], row[SSTV_LINE_BYTES];
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        const uint8_t* ref = (y % 2 == 0) ? NULL : prev; /* blocks of 2 lines: first line has no ref */
        int n = sstv_codec_encode(img + y * SSTV_LINE_BYTES, ref, rec);
        total += n;
        int m = sstv_codec_decode(rec, n, ref, row);
        if(m != n || memcmp(row, img + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES) != 0) bad++;
        memcpy(prev, img + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES);
    }
    free(buf);
    *total_out = total;
    return bad;
}

int main(void) {
    /* synthetic rows */
    uint8_t row[SSTV_LINE_BYTES], out[SSTV_LINE_BYTES], rec[SSTV_CODEC_MAX_RECORD];
    memset(row, 0, sizeof row);
    int n = sstv_codec_encode(row, NULL, rec); /* all white: runs 255,0,145 -> 3 bytes */
    CHECK(n <= 5);
    CHECK_EQ_INT(sstv_codec_decode(rec, n, NULL, out), n);
    CHECK(memcmp(out, row, sizeof row) == 0);

    memset(row, 0xFF, sizeof row); /* all black */
    n = sstv_codec_encode(row, NULL, rec);
    CHECK(n <= 6);
    CHECK_EQ_INT(sstv_codec_decode(rec, n, NULL, out), n);
    CHECK(memcmp(out, row, sizeof row) == 0);

    for(int i = 0; i < SSTV_LINE_BYTES; i++) row[i] = (uint8_t)(i * 97 + 13); /* noise: raw */
    n = sstv_codec_encode(row, NULL, rec);
    CHECK_EQ_INT(n, 1 + SSTV_LINE_BYTES);
    CHECK_EQ_INT(rec[0] >> 6, SSTV_CODEC_RAW);
    CHECK_EQ_INT(sstv_codec_decode(rec, n, NULL, out), n);
    CHECK(memcmp(out, row, sizeof row) == 0);

    /* identical to reference: XOR mode, tiny */
    n = sstv_codec_encode(row, row, rec);
    CHECK(n <= 5);
    CHECK_EQ_INT(rec[0] >> 6, SSTV_CODEC_XOR_RLE);
    CHECK_EQ_INT(sstv_codec_decode(rec, n, row, out), n);
    CHECK(memcmp(out, row, sizeof row) == 0);

    /* truncated record is rejected */
    CHECK_EQ_INT(sstv_codec_decode(rec, n - 1, row, out), -1);

    /* sample images round-trip and report ratios */
    const char* files[] = {"../images/sphere.pbm", "../images/test_pattern.pbm", "../images/checker.pbm"};
    for(int i = 0; i < 3; i++) {
        int total = 0;
        int bad = roundtrip_image(files[i], &total);
        if(bad < 0) continue; /* not present */
        CHECK_EQ_INT(bad, 0);
        printf("  %s: %d bytes (%.2fx)\n", files[i], total, (double)SSTV_IMAGE_BYTES / total);
    }
    TEST_MAIN_END();
}
