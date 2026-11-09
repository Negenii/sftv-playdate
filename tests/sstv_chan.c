/* Channel stress test for the HF band: an image pass through a modelled acoustic path (per-tone
 * microphone response, room echo, noise, a clock offset between the devices), decoded by the
 * demodulator + receiver. Prints how much arrives.
 *   bin/sstv_chan [profile] [noise_lsb] [echo] [ppm] [floor_lsb]
 * profile: flat | laptop (the 2026-11-07 browser recording: 17/17.5 kHz ~20 dB under 19.5 kHz) */
#include "sstv_demod.h"
#include "sstv_frame.h"
#include "sstv_modem_ref.h"
#include "sstv_rx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t seen[SSTV_HEIGHT];
static void on_pixels(void* c, uint16_t line, int xb, int nb, const uint8_t* px) {
    (void)c; (void)px;
    if(line < SSTV_HEIGHT) seen[line] |= nb == SSTV_LINE_BYTES ? 3 : (xb == 0 ? 1 : 2);
}
static void on_meta(void* c, const char* nm, bool end, int frames, bool coded) { (void)c; (void)nm; (void)end; (void)frames; (void)coded; }
static void on_preview(void* c, int l, int ch, int nb, const uint8_t* d) { (void)c; (void)l; (void)ch; (void)nb; (void)d; }
static sstv_rx rx;
static int conf[8][8], lock_mode;
static void on_phys(void* c, const uint8_t* f, const float* soft) {
    (void)c;
    if(lock_mode) {
        uint8_t ref[SSTV_FRAME_BYTES];
        sstv_lock_frame(ref);
        if(sstv_lock_distance(f) < 120)
            for(int i = 0; i < 109; i++) conf[sstv_frame_symbol_bits(ref, i, 3)][sstv_frame_symbol_bits(f, i, 3)]++;
    }
    sstv_rx_phys(&rx, f, soft);
}

static double gauss(uint32_t* x) {
    double s = 0;
    for(int i = 0; i < 12; i++) {
        *x = *x * 1103515245u + 12345u;
        s += (double)(*x >> 8) / 16777216.0;
    }
    return s - 6.0;
}

int main(int argc, char** argv) {
    const char* profile = argc > 1 ? argv[1] : "flat";
    double noise = argc > 2 ? atof(argv[2]) : 2.0;
    double echo = argc > 3 ? atof(argv[3]) : 0.0;
    double ppm = argc > 4 ? atof(argv[4]) : 0.0;
    double floor_lsb = argc > 5 ? atof(argv[5]) : -1.0;
    const float sr = 48000.0f;
    uint8_t* img = calloc(1, SSTV_IMAGE_BYTES);
    uint32_t r = 7;
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) { /* noisy picture: raw mode, every symbol value */
        r = r * 1103515245u + 12345u;
        img[i] = (uint8_t)(r >> 16);
    }
    sstv_mod m;
    sstv_mod_init(&m, sr * (float)(1.0 + ppm * 1e-6), 40); /* the sender's clock runs off by ppm */
    sstv_mod_set_band(&m, SSTV_BAND_HF);
    if(strcmp(profile, "laptop") == 0) {
        static const float g[8] = {0.07f, 0.08f, 0.47f, 0.43f, 0.82f, 1.0f, 0.47f, 0.33f}; /* peak amplitudes 3.6..55 */
        for(int k = 0; k < 8; k++) m.channel_gain[k] = g[k] * 1.4f;
    }
    int cap = sstv_mod_image_capacity(sr) * 2;
    int16_t* pcm = calloc((size_t)cap, sizeof(int16_t));
    int n = 24000;
    int k;
    lock_mode = getenv("LOCK") != NULL;
    if(lock_mode) { /* the preamble, then lock frames only (known symbols) */
        k = sstv_mod_preamble(&m, pcm + n, cap - n);
        n += k;
        uint8_t lk[SSTV_FRAME_BYTES];
        sstv_lock_frame(lk);
        for(int f = 0; f < 100; f++) n += sstv_mod_frame(&m, lk, pcm + n, cap - n);
        n += 24000;
    } else {
        k = sstv_mod_image(&m, img, pcm + n, cap - n);
        if(k < 0) { puts("capacity"); return 1; }
        n += k + 24000;
    }
    /* room: echoes at 1.3 ms and 3.7 ms */
    float* y = calloc((size_t)n, sizeof(float));
    int e1 = (int)(0.0013 * (double)sr), e2 = (int)(0.0037 * (double)sr);
    for(int i = 0; i < n; i++) {
        double v = (double)pcm[i];
        if(echo > 0) {
            if(i >= e1) v += echo * pcm[i - e1];
            if(i >= e2) v += 0.6 * echo * pcm[i - e2];
        }
        y[i] = (float)v;
    }
    uint32_t gx = 99;
    for(int i = 0; i < n; i++) {
        double v = (double)y[i] + noise * gauss(&gx);
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    static sstv_demod d;
    sstv_rx_init(&rx, on_pixels, on_meta, on_preview, NULL);
    sstv_demod_init(&d, sr, on_phys, NULL);
    sstv_demod_set_band(&d, SSTV_BAND_HF);
    if(floor_lsb > 0) {
        float fa = (float)floor_lsb * 0.6366f * (float)d.window * 32767.0f;
        d.min_energy = fa * fa;
        for(int t = 0; t < SSTV_MAX_TONES; t++) d.gain[t] = d.min_energy;
    }
    for(int i = 0; i < n; i += 512) sstv_demod_feed(&d, pcm + i, n - i < 512 ? n - i : 512);
    int lines = 0;
    for(int t = 0; t < SSTV_HEIGHT; t++) lines += seen[t] == 3;
    printf("%-6s noise %.1f echo %.2f ppm %+.0f floor %.1f: lines %3d/240  frames %d ok %d bad %d repaired %d  syncs %d flywheel %d\n",
           profile, noise, echo, ppm, floor_lsb, lines, d.frames, rx.frames_ok, rx.frames_bad, rx.frames_repaired, d.patterns_seen,
           d.flywheel_frames);
    if(lock_mode) {
        puts("sent \\ decided  0..7 (17.0 .. 20.5 kHz)");
        for(int a = 0; a < 8; a++) {
            int tot = 0;
            for(int b = 0; b < 8; b++) tot += conf[a][b];
            printf("  %d:", a);
            for(int b = 0; b < 8; b++) printf(" %5d", conf[a][b]);
            printf("   err %.1f%%\n", tot ? 100.0 * (tot - conf[a][a]) / tot : 0.0);
        }
    }
    return 0;
}
