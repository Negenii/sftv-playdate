#include "wav_io.h"
#include "sstv_demod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t image[SSTV_IMAGE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
    int distinct;
} decoder_ctx;

static void on_line(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok) {
    decoder_ctx* c = ctx;
    (void)crc_ok;
    if(line_no >= SSTV_HEIGHT) return;
    memcpy(c->image + line_no * SSTV_LINE_BYTES, pixels, SSTV_LINE_BYTES);
    if(!c->seen[line_no]) {
        c->seen[line_no] = 1;
        c->distinct++;
    }
}

int main(int argc, char** argv) {
    if(argc < 3) {
        fprintf(stderr, "usage: sstv_decode_wav in.wav out.pbm\n");
        return 2;
    }
    int n, sr;
    int16_t* s = wav_read_mono16(argv[1], &n, &sr);
    if(!s) {
        fprintf(stderr, "%s: cannot read 16-bit PCM WAV\n", argv[1]);
        return 1;
    }

    decoder_ctx* c = calloc(1, sizeof *c);
    sstv_demod d;
    sstv_demod_init(&d, (float)sr, on_line, c);
    sstv_demod_feed(&d, s, n);

    sstv_demod_status_t st;
    sstv_demod_status(&d, &st);
    printf("lines=%d distinct=%d crc_errors=%d missing=%d sample_rate=%d\n", st.lines, c->distinct,
           st.crc_errors, SSTV_HEIGHT - c->distinct, sr);

    FILE* f = fopen(argv[2], "wb");
    if(!f) {
        perror(argv[2]);
        return 1;
    }
    fprintf(f, "P4\n%d %d\n", SSTV_WIDTH, SSTV_HEIGHT);
    fwrite(c->image, 1, SSTV_IMAGE_BYTES, f);
    fclose(f);
    free(c);
    free(s);
    return 0;
}
