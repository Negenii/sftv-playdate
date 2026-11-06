/* Near-ultrasound band: the reference modulator on the HF tones (8-FSK: 7 tones 17-20.5 kHz + silence, sync 21 kHz, 3 ms symbols, 3 bits each)
 * at the level the Playdate mic measured for the Flipper buzzer (~40 LSB, noise ~1-2 LSB), a
 * receiver tuned with sstv_demod_set_band(SSTV_BAND_HF) at 44.1 kHz must get the whole picture,
 * the file name included; and a receiver left on the audible band must get nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_util.h"
#include "sstv_demod.h"
#include "sstv_frame.h"
#include "sstv_modem_ref.h"
#include "sstv_rx.h"

static uint8_t got[SSTV_HEIGHT][SSTV_LINE_BYTES];
static uint8_t seen[SSTV_HEIGHT];
static char name[64];
static void on_pixels(void* c, uint16_t line, int xb, int nb, const uint8_t* px) {
    (void)c;
    if(line >= SSTV_HEIGHT) return;
    memcpy(got[line] + xb, px, (size_t)nb);
    seen[line] |= nb == SSTV_LINE_BYTES ? 3 : (xb == 0 ? 1 : 2);
}
static void on_meta(void* c, const char* nm, bool end, int frames, bool coded) {
    (void)c; (void)frames; (void)coded;
    if(!end) snprintf(name, sizeof name, "%s", nm);
}
static void on_preview(void* c, int layer, int chunk, int nb, const uint8_t* d) { (void)c; (void)layer; (void)chunk; (void)nb; (void)d; }
static sstv_rx rx;
static void on_phys(void* c, const uint8_t* frame, const float* soft) { (void)c; sstv_rx_phys(&rx, frame, soft); }

static int run(const int16_t* pcm, int n, int band) {
    memset(seen, 0, sizeof seen);
    name[0] = 0;
    static sstv_demod d;
    sstv_rx_init(&rx, on_pixels, on_meta, on_preview, NULL);
    sstv_demod_init(&d, 44100.0f, on_phys, NULL);
    sstv_demod_set_band(&d, band);
    for(int i = 0; i < n; i += 512) sstv_demod_feed(&d, pcm + i, n - i < 512 ? n - i : 512);
    int lines = 0;
    for(int y = 0; y < SSTV_HEIGHT; y++) lines += seen[y] == 3;
    return lines;
}

int main(void) {
    uint8_t* img = calloc(1, SSTV_IMAGE_BYTES);
    for(int y = 0; y < SSTV_HEIGHT; y++)
        for(int i = 0; i < SSTV_LINE_BYTES; i++) img[y * SSTV_LINE_BYTES + i] = (uint8_t)(((y >> 3) & 1) ? 0xF0 : 0x0F);
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, 40); /* ~40 LSB square wave, as measured */
    sstv_mod_set_band(&m, SSTV_BAND_HF);
    int cap = sstv_mod_image_capacity(44100.0f);
    int16_t* pcm = malloc(sizeof(int16_t) * (size_t)(cap + 44100));
    memset(pcm, 0, sizeof(int16_t) * 22050);
    int n = sstv_mod_image(&m, img, pcm + 22050, cap);
    CHECK(n > 0);
    n += 22050;
    uint32_t x = 1;
    for(int i = 0; i < n; i++) { /* +-2 LSB noise */
        x = x * 1103515245u + 12345u;
        pcm[i] = (int16_t)(pcm[i] + (int)((x >> 16) % 5) - 2);
    }
    int lines = run(pcm, n, SSTV_BAND_HF);
    printf("  hf band: %d/240 lines, name \"%s\"\n", lines, name);
    CHECK_EQ_INT(lines, SSTV_HEIGHT);
    CHECK(strcmp(name, "image") == 0);
    for(int y = 0; y < SSTV_HEIGHT; y++) CHECK(memcmp(got[y], img + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0);
    int lines_aud = run(pcm, n, SSTV_BAND_AUDIBLE);
    printf("  audible-band receiver on the hf signal: %d lines\n", lines_aud);
    CHECK_EQ_INT(lines_aud, 0);
    free(pcm);
    free(img);
    TEST_MAIN_END();
}
