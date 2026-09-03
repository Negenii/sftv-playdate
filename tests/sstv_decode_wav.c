#include "wav_io.h"
#include "sstv_demod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t image[SSTV_IMAGE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
    int distinct;
    int good; /* CRC-ok lines */
} decoder_ctx;

static sstv_demod d;
static FILE* dump; /* SSTV_DUMP=path: per line "line_no crc_ok margin[416]" */

static void on_line(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok, const float* soft, int corrected) {
    (void)soft;
    (void)corrected;
    decoder_ctx* c = ctx;
    if(crc_ok) c->good++;
    if(dump) {
        fprintf(dump, "%d %d", line_no, crc_ok ? 1 : 0);
        for(int i = 0; i < SSTV_FRAME_BITS; i++) fprintf(dump, " %.4f", (double)d.margin[i]);
        fprintf(dump, "\n");
    }
    if(line_no >= SSTV_HEIGHT) return;
    memcpy(c->image + line_no * SSTV_LINE_BYTES, pixels, SSTV_LINE_BYTES);
    if(!c->seen[line_no]) {
        c->seen[line_no] = 1;
        c->distinct++;
    }
}

int main(int argc, char** argv) {
    if(argc < 3) {
        fprintf(stderr, "usage: sstv_decode_wav in.wav out.pbm [sample_point] [edge_adjust]\n");
        return 2;
    }
    int n, sr;
    int16_t* s = wav_read_mono16(argv[1], &n, &sr);
    if(!s) {
        fprintf(stderr, "%s: cannot read 16-bit PCM WAV\n", argv[1]);
        return 1;
    }

    decoder_ctx* c = calloc(1, sizeof *c);
    if(getenv("SSTV_DUMP")) dump = fopen(getenv("SSTV_DUMP"), "w");
    sstv_demod_init(&d, (float)sr, on_line, c);
    if(argc > 3) d.sample_point = (float)atof(argv[3]); /* tuning experiments */
    if(argc > 4) d.edge_adjust = (float)atof(argv[4]);
    sstv_demod_feed(&d, s, n);

    sstv_demod_status_t st;
    sstv_demod_status(&d, &st);
    printf("lines=%d good=%d distinct=%d crc_errors=%d rs_fixed=%d missing=%d sample_rate=%d\n", st.lines,
           c->good, c->distinct, st.crc_errors, st.rs_corrected, SSTV_HEIGHT - c->distinct, sr);

    FILE* f = fopen(argv[2], "wb");
    if(!f) {
        perror(argv[2]);
        return 1;
    }
    fprintf(f, "P4\n%d %d\n", SSTV_WIDTH, SSTV_HEIGHT);
    fwrite(c->image, 1, SSTV_IMAGE_BYTES, f);
    fclose(f);
    if(dump) fclose(dump);
    free(c);
    free(s);
    return 0;
}
