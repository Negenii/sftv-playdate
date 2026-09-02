#include "wav_io.h"
#include "pbm.h"
#include "sstv_modem_ref.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if(argc < 3) {
        fprintf(stderr, "usage: sstv_wav in.pbm out.wav [sample_rate]\n");
        return 2;
    }
    int sr = argc > 3 ? atoi(argv[3]) : 44100;
    FILE* f = fopen(argv[1], "rb");
    if(!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* pbm = malloc((size_t)size);
    if(fread(pbm, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        return 1;
    }
    fclose(f);

    int w, h;
    size_t off;
    if(pbm_parse_header(pbm, (size_t)size, &w, &h, &off) != 0 || w != SSTV_WIDTH ||
       h != SSTV_HEIGHT || (size_t)size < off + SSTV_IMAGE_BYTES) {
        fprintf(stderr, "%s: need a P4 PBM of exactly %dx%d\n", argv[1], SSTV_WIDTH, SSTV_HEIGHT);
        return 1;
    }

    sstv_mod m;
    sstv_mod_init(&m, (float)sr, 16000);
    int lead = sr / 2; /* 0.5 s silence so a player's start does not clip the leader */
    int cap = lead + sstv_mod_image_capacity((float)sr) + sr / 2;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    int n = sstv_mod_image(&m, pbm + off, s + lead, cap - lead);
    if(n < 0) {
        fprintf(stderr, "modulation overflow\n");
        return 1;
    }
    n += lead + sr / 2;
    if(wav_write_mono16(argv[2], s, n, sr) != 0) {
        perror(argv[2]);
        return 1;
    }
    printf("wrote %s: %d samples, %.1f s at %d Hz\n", argv[2], n, (double)n / sr, sr);
    free(s);
    free(pbm);
    return 0;
}
