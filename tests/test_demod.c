#include "test_util.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_modem_ref.h"
#include "sstv_frame.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AMP 8000

/* demod -> rx -> segments */
typedef struct {
    sstv_rx rx;
    int count; /* segments delivered */
    int ok;
    uint8_t got[SSTV_NUM_SEGS][SSTV_SEG_BYTES];
    uint8_t seen[SSTV_NUM_SEGS]; /* 1 delivered bad, 2 delivered ok */
} capture;

static void on_seg(void* ctx, uint16_t seg, const uint8_t* pixels, bool ok, int corrected) {
    (void)corrected;
    capture* c = ctx;
    c->count++;
    if(ok) c->ok++;
    if(seg < SSTV_NUM_SEGS) {
        if(ok || !c->seen[seg]) memcpy(c->got[seg], pixels, SSTV_SEG_BYTES);
        if(ok) c->seen[seg] = 2;
        else if(!c->seen[seg]) c->seen[seg] = 1;
    }
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    capture* c = ctx;
    sstv_rx_phys(&c->rx, frame, soft);
}

static capture* capture_new(void) {
    capture* c = calloc(1, sizeof *c);
    sstv_rx_init(&c->rx, on_seg, c);
    return c;
}

/* Deterministic pseudo random bytes. */
static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}
static void fill_random(uint8_t* p, int n) {
    for(int i = 0; i < n; i++) p[i] = (uint8_t)(rng() >> 24);
}

/* Gaussian noise with standard deviation sigma (Box-Muller). */
static void add_noise(int16_t* s, int n, float sigma) {
    for(int i = 0; i + 1 < n; i += 2) {
        float u1 = ((float)(rng() >> 8) + 1.0f) / 16777217.0f;
        float u2 = ((float)(rng() >> 8) + 1.0f) / 16777217.0f;
        float r = sigma * sqrtf(-2.0f * logf(u1));
        float a = 6.2831853f * u2;
        float z0 = r * cosf(a), z1 = r * sinf(a);
        int v0 = s[i] + (int)z0, v1 = s[i + 1] + (int)z1;
        s[i] = (int16_t)(v0 > 32767 ? 32767 : v0 < -32768 ? -32768 : v0);
        s[i + 1] = (int16_t)(v1 > 32767 ? 32767 : v1 < -32768 ? -32768 : v1);
    }
}

static int silence_samples(float sr, int ms) {
    return (int)(sr * (float)ms / 1000.0f);
}

/* Feeds in chunks of varying size so chunk boundaries are exercised. */
static void feed_chunked(sstv_demod* d, const int16_t* s, int n) {
    int sizes[] = {1, 7, 64, 512, 1000, 3};
    int i = 0, k = 0;
    while(i < n) {
        int c = sizes[k++ % 6];
        if(i + c > n) c = n - i;
        sstv_demod_feed(d, s + i, c);
        i += c;
    }
}

static int segs_matching(const capture* c, const uint8_t* image) {
    int ok = 0;
    for(int s = 0; s < SSTV_NUM_SEGS; s++) {
        const uint8_t* truth = image + sstv_seg_line(s) * SSTV_LINE_BYTES + sstv_seg_byte_offset(s);
        if(c->seen[s] == 2 && memcmp(c->got[s], truth, SSTV_SEG_BYTES) == 0) ok++;
    }
    return ok;
}

/* Modulates a whole random image, optionally adds noise (continuous sigma and/or periodic
 * bursts), demodulates at demod_sr, returns segments matching. */
typedef struct {
    float sigma; /* continuous white noise */
    float burst_sigma; /* burst noise level */
    int burst_ms; /* burst length */
    int burst_period_ms; /* burst repetition */
} noise_spec;

static int run_image(sstv_mod* m, float demod_sr, const noise_spec* ns, capture* c, uint8_t* image) {
    fill_random(image, SSTV_IMAGE_BYTES);
    float sr = m->sample_rate;
    int cap = sstv_mod_image_capacity(sr) + silence_samples(sr, 100);
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    int pre = silence_samples(sr, 50);
    int n = pre + sstv_mod_image(m, image, s + pre, cap - pre);
    n += silence_samples(sr, 50);
    if(ns) {
        if(ns->sigma > 0) add_noise(s, n, ns->sigma);
        if(ns->burst_sigma > 0) {
            int period = silence_samples(sr, ns->burst_period_ms), len = silence_samples(sr, ns->burst_ms);
            for(int start = period / 2; start + len < n; start += period) add_noise(s + start, len, ns->burst_sigma);
        }
    }
    sstv_demod d;
    sstv_demod_init(&d, demod_sr, on_phys, c);
    feed_chunked(&d, s, n);
    free(s);
    if(ns) printf("    (physical frames %d, flywheel %d)\n", d.frames, d.flywheel_frames);
    return segs_matching(c, image);
}

static void test_single_block(void) {
    /* One interleave block of random segments, preceded by a short leader. */
    float sr = 44100.0f;
    uint8_t* image = calloc(SSTV_IMAGE_BYTES, 1);
    fill_random(image, SSTV_IMAGE_BYTES);
    uint8_t* phys = malloc(SSTV_NUM_SEGS * SSTV_FRAME_BYTES);
    sstv_build_physical(image, phys);
    int cap = silence_samples(sr, 200) + SSTV_ILEAVE * 30000;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = silence_samples(sr, 20);
    n += sstv_mod_tone(&m, m.tones[1], 50000, s + n, cap - n);
    for(int p = 0; p < SSTV_ILEAVE; p++) n += sstv_mod_frame(&m, phys + p * SSTV_FRAME_BYTES, s + n, cap - n);
    n += silence_samples(sr, 20);

    capture* c = capture_new();
    sstv_demod d;
    sstv_demod_init(&d, sr, on_phys, c);
    feed_chunked(&d, s, n);
    CHECK_EQ_INT(d.frames, SSTV_ILEAVE);
    CHECK_EQ_INT(c->count, SSTV_ILEAVE);
    CHECK_EQ_INT(c->ok, SSTV_ILEAVE);
    CHECK_EQ_INT(segs_matching(c, image), SSTV_ILEAVE);
    /* the flywheel may already be waiting for a next frame; it must not have produced one */
    CHECK_EQ_INT(d.frames, SSTV_ILEAVE);
    free(s);
    free(phys);
    free(image);
    free(c);
}

static void test_full_image_clean(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    int ok = run_image(&m, 44100.0f, NULL, c, image);
    CHECK_EQ_INT(c->count, SSTV_NUM_SEGS);
    CHECK_EQ_INT(ok, SSTV_NUM_SEGS);
    free(c);
    free(image);
}

static void test_noise(const noise_spec* ns, const char* label, int min_ok) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    int ok = run_image(&m, 44100.0f, ns, c, image);
    printf("  %s: %d/%d segments intact, %d delivered, rs fixed %d bytes\n", label, ok, SSTV_NUM_SEGS, c->count,
           c->rx.rs_corrected);
    CHECK(ok >= min_ok);
    free(c);
    free(image);
}

static void test_frequency_offset(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    for(int k = 0; k < SSTV_NUM_TONES; k++) m.tones[k] *= 1.015f; /* 0 stays 0 */
    m.fsync *= 1.015f;
    CHECK_EQ_INT(run_image(&m, 44100.0f, NULL, c, image), SSTV_NUM_SEGS);
    free(c);
    free(image);
}

static void test_clock_offset(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    for(int sign = -1; sign <= 1; sign += 2) {
        capture* c = capture_new();
        sstv_mod m;
        sstv_mod_init(&m, 44100.0f * (1.0f + sign * 0.0015f), AMP);
        int ok = run_image(&m, 44100.0f, NULL, c, image);
        printf("  clock %+d ppm: %d/%d segments intact\n", sign * 1500, ok, SSTV_NUM_SEGS);
        CHECK(ok >= SSTV_NUM_SEGS * 95 / 100); /* 1500 ppm is 1.5x the measured Flipper/Playdate offset */
        free(c);
    }
    free(image);
}

static void test_other_sample_rate(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 22050.0f, AMP);
    CHECK_EQ_INT(run_image(&m, 22050.0f, NULL, c, image), SSTV_NUM_SEGS);
    free(c);
    free(image);
}

static void test_lost_sync(void) {
    /* Two blocks; the second physical frame of block 0 loses most of its sync. The
     * flywheel must start that frame from the predicted edge, so both blocks survive. */
    float sr = 44100.0f;
    uint8_t* image = calloc(SSTV_IMAGE_BYTES, 1);
    fill_random(image, SSTV_IMAGE_BYTES);
    uint8_t* phys = malloc(SSTV_NUM_SEGS * SSTV_FRAME_BYTES);
    sstv_build_physical(image, phys);
    int cap = 2 * SSTV_ILEAVE * 30000 + 20000;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = silence_samples(sr, 20);
    int start[2 * SSTV_ILEAVE];
    for(int p = 0; p < 2 * SSTV_ILEAVE; p++) {
        start[p] = n;
        n += sstv_mod_frame(&m, phys + p * SSTV_FRAME_BYTES, s + n, cap - n);
    }
    n += silence_samples(sr, 20);
    int cut_ms = SSTV_SYNC_US / 1000 - 2;
    memset(s + start[1], 0, sizeof(int16_t) * (size_t)silence_samples(sr, cut_ms));

    capture* c = capture_new();
    sstv_demod d;
    sstv_demod_init(&d, sr, on_phys, c);
    feed_chunked(&d, s, n);
    int ok0 = 0, ok1 = 0;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        ok0 += c->seen[i] == 2;
        ok1 += c->seen[SSTV_ILEAVE + i] == 2;
    }
    CHECK_EQ_INT(ok0, SSTV_ILEAVE);
    CHECK_EQ_INT(ok1, SSTV_ILEAVE);
    CHECK(d.flywheel_frames >= 1);
    free(s);
    free(phys);
    free(image);
    free(c);
}

static void test_silence_and_reset(void) {
    sstv_demod d;
    sstv_demod_init(&d, 44100.0f, on_phys, NULL);
    int16_t zeros[4410] = {0};
    sstv_demod_feed(&d, zeros, 4410);
    sstv_demod_status_t st;
    sstv_demod_status(&d, &st);
    CHECK_EQ_INT(st.tone, -1);
    CHECK(st.level < 0.01f);
    CHECK_EQ_INT(st.frames, 0);
}

int main(void) {
    test_single_block();
    test_full_image_clean();
    noise_spec white6 = {(float)AMP / 2.0f, 0, 0, 0};
    noise_spec white0 = {(float)AMP, 0, 0, 0};
    /* speech-like: 150 ms bursts at 6x the signal amplitude every 2 s, on top of 12 dB SNR */
    noise_spec bursts = {(float)AMP / 4.0f, (float)AMP * 6.0f, 150, 2000};
    /* long bursts: 400 ms every 3 s */
    noise_spec long_bursts = {(float)AMP / 4.0f, (float)AMP * 6.0f, 400, 3000};
    test_noise(&white6, "white 6 dB", SSTV_NUM_SEGS);
    test_noise(&white0, "white 0 dB", SSTV_NUM_SEGS * 95 / 100);
    test_noise(&bursts, "150 ms bursts / 2 s", SSTV_NUM_SEGS * 97 / 100);
    test_noise(&long_bursts, "400 ms bursts / 3 s", SSTV_NUM_SEGS * 80 / 100);
    noise_spec word_bursts = {(float)AMP / 4.0f, (float)AMP * 6.0f, 700, 3000};
    test_noise(&word_bursts, "700 ms bursts / 3 s", SSTV_NUM_SEGS * 50 / 100);
    test_frequency_offset();
    test_clock_offset();
    test_other_sample_rate();
    test_lost_sync();
    test_silence_and_reset();
    TEST_MAIN_END();
}
