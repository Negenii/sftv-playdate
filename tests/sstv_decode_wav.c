#include "wav_io.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_frame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sstv_rx rx;
    uint8_t image[SSTV_IMAGE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
    int good;
} decoder_ctx;

static FILE* dump; /* SSTV_DUMP=path: per physical frame "hex bytes | soft values" */
static sstv_demod* g_demod;

static void on_line(void* ctx, uint16_t line, const uint8_t* pixels) {
    decoder_ctx* c = ctx;
    if(line >= SSTV_HEIGHT) return;
    memcpy(c->image + line * SSTV_LINE_BYTES, pixels, SSTV_LINE_BYTES);
    if(!c->seen[line]) c->good++;
    c->seen[line] = 1;
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    decoder_ctx* c = ctx;
    if(dump) {
        for(int i = 0; i < SSTV_FRAME_BYTES; i++) fprintf(dump, "%02x", frame[i]);
        fprintf(dump, " |");
        for(int i = 0; i < SSTV_FRAME_BITS; i++) fprintf(dump, " %.3f", (double)soft[i]);
        fprintf(dump, "\n");
        if(getenv("SSTV_DUMP_ENERGY")) {
            fprintf(dump, "E");
            for(int i = 0; i < SSTV_SYMBOLS_PER_FRAME; i++)
                for(int k = 0; k < SSTV_NUM_TONES; k++) fprintf(dump, " %.4f", (double)g_demod->dbg_nrm[i][k]);
            fprintf(dump, "\n");
        }
    }
    sstv_rx_phys(&c->rx, frame, soft);
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
    sstv_rx_init(&c->rx, on_line, c);
    if(getenv("SSTV_DUMP")) dump = fopen(getenv("SSTV_DUMP"), "w");
    sstv_demod d;
    g_demod = &d;
    sstv_demod_init(&d, (float)sr, on_phys, c);
    if(argc > 3) d.sample_point = (float)atof(argv[3]); /* tuning experiments */
    if(argc > 4) d.edge_adjust = (float)atof(argv[4]);
    if(argc > 5) d.silence_level = (float)atof(argv[5]);
    sstv_demod_feed(&d, s, n);

    printf("phys_frames=%d flywheel=%d blocks=%d frames_ok=%d frames_bad=%d lines=%d/%d rs_fixed=%d sample_rate=%d\n",
           d.frames, d.flywheel_frames, c->rx.blocks_ok, c->rx.frames_ok, c->rx.frames_bad, c->good, SSTV_HEIGHT,
           c->rx.rs_corrected, sr);

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
