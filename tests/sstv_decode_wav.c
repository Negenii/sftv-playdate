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
    uint8_t seen[SSTV_NUM_SEGS]; /* 1 bad copy, 2 good copy */
    int good;
    int distinct;
} decoder_ctx;

static FILE* dump; /* SSTV_DUMP=path: per physical frame "hex bytes | soft values" */

static void on_seg(void* ctx, uint16_t seg, const uint8_t* pixels, bool ok, int corrected) {
    (void)corrected;
    decoder_ctx* c = ctx;
    if(seg >= SSTV_NUM_SEGS) return;
    if(c->seen[seg] == 2 && !ok) return;
    memcpy(c->image + sstv_seg_line(seg) * SSTV_LINE_BYTES + sstv_seg_byte_offset(seg), pixels, SSTV_SEG_BYTES);
    if(!c->seen[seg]) c->distinct++;
    if(ok && c->seen[seg] != 2) c->good++;
    c->seen[seg] = ok ? 2 : 1;
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    decoder_ctx* c = ctx;
    if(dump) {
        for(int i = 0; i < SSTV_FRAME_BYTES; i++) fprintf(dump, "%02x", frame[i]);
        fprintf(dump, " |");
        for(int i = 0; i < SSTV_FRAME_BITS; i++) fprintf(dump, " %.3f", (double)soft[i]);
        fprintf(dump, "\n");
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
    sstv_rx_init(&c->rx, on_seg, c);
    if(getenv("SSTV_DUMP")) dump = fopen(getenv("SSTV_DUMP"), "w");
    sstv_demod d;
    sstv_demod_init(&d, (float)sr, on_phys, c);
    if(argc > 3) d.sample_point = (float)atof(argv[3]); /* tuning experiments */
    if(argc > 4) d.edge_adjust = (float)atof(argv[4]);
    sstv_demod_feed(&d, s, n);

    printf("phys_frames=%d flywheel=%d blocks=%d segs_ok=%d segs_bad=%d good=%d/%d distinct=%d rs_fixed=%d sample_rate=%d\n",
           d.frames, d.flywheel_frames, c->rx.blocks_ok, c->rx.segs_ok, c->rx.segs_bad, c->good, SSTV_NUM_SEGS,
           c->distinct, c->rx.rs_corrected, sr);

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
