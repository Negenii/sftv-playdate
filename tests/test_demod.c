#include "test_util.h"
#include "sstv_demod.h"
#include "sstv_modem_ref.h"
#include "sstv_frame.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AMP 8000

typedef struct {
    int count;
    int crc_ok;
    int in_order;
    uint8_t last_line;
    uint8_t got[SSTV_HEIGHT][SSTV_LINE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
} capture;

static void on_line(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok) {
    capture* c = ctx;
    if(c->count > 0 && line_no == (uint8_t)(c->last_line + 1)) c->in_order++;
    c->last_line = line_no;
    c->count++;
    if(crc_ok) c->crc_ok++;
    if(line_no < SSTV_HEIGHT) {
        memcpy(c->got[line_no], pixels, SSTV_LINE_BYTES);
        c->seen[line_no] = 1;
    }
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

static void test_single_line(void) {
    uint8_t pixels[SSTV_LINE_BYTES], frame[SSTV_FRAME_BYTES];
    fill_random(pixels, SSTV_LINE_BYTES);
    sstv_frame_pack(7, pixels, frame);

    float sr = 44100.0f;
    int pre = silence_samples(sr, 20), post = silence_samples(sr, 20);
    int cap = pre + 4000 + 40000 + post;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = pre;
    n += sstv_mod_tone(&m, m.f1, 50000, s + n, cap - n); /* short leader */
    n += sstv_mod_frame(&m, frame, s + n, cap - n);
    n += post;

    capture c = {0};
    sstv_demod d;
    sstv_demod_init(&d, sr, on_line, &c);
    feed_chunked(&d, s, n);

    CHECK_EQ_INT(c.count, 1);
    CHECK_EQ_INT(c.crc_ok, 1);
    CHECK_EQ_INT(c.last_line, 7);
    CHECK(memcmp(c.got[7], pixels, SSTV_LINE_BYTES) == 0);

    sstv_demod_status_t st;
    sstv_demod_status(&d, &st);
    CHECK_EQ_INT(st.lines, 1);
    CHECK_EQ_INT(st.crc_errors, 0);
    CHECK(st.state == SSTV_DEMOD_IDLE);
    free(s);
}

/* Modulates a whole random image with the given modulator settings, demodulates at
 * demod_sr, returns the capture. */
static void run_image(sstv_mod* m, float demod_sr, float noise_sigma, capture* c, uint8_t* image) {
    fill_random(image, SSTV_IMAGE_BYTES);
    int cap = sstv_mod_image_capacity(m->sample_rate) + silence_samples(m->sample_rate, 100);
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    int pre = silence_samples(m->sample_rate, 50);
    int n = pre + sstv_mod_image(m, image, s + pre, cap - pre);
    n += silence_samples(m->sample_rate, 50);
    if(noise_sigma > 0) add_noise(s, n, noise_sigma);

    memset(c, 0, sizeof *c);
    sstv_demod d;
    sstv_demod_init(&d, demod_sr, on_line, c);
    feed_chunked(&d, s, n);
    free(s);
}

static int lines_matching(const capture* c, const uint8_t* image) {
    int ok = 0;
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        if(c->seen[y] && memcmp(c->got[y], image + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0) ok++;
    }
    return ok;
}

static void test_full_image_clean(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = malloc(sizeof *c);
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    run_image(&m, 44100.0f, 0.0f, c, image);
    CHECK_EQ_INT(c->count, SSTV_HEIGHT);
    CHECK_EQ_INT(c->crc_ok, SSTV_HEIGHT);
    CHECK_EQ_INT(c->in_order, SSTV_HEIGHT - 1);
    CHECK_EQ_INT(lines_matching(c, image), SSTV_HEIGHT);
    free(c);
    free(image);
}

static void test_full_image_noise(float sigma, const char* label, int min_ok) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = malloc(sizeof *c);
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    run_image(&m, 44100.0f, sigma, c, image);
    int ok = lines_matching(c, image);
    printf("  noise %s: %d/%d lines intact, %d received, %d crc errors\n",
           label, ok, SSTV_HEIGHT, c->count, c->count - c->crc_ok);
    CHECK(ok >= min_ok);
    free(c);
    free(image);
}

static void test_frequency_offset(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = malloc(sizeof *c);
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    m.f0 *= 1.015f;
    m.f1 *= 1.015f;
    m.fsync *= 1.015f;
    run_image(&m, 44100.0f, 0.0f, c, image);
    CHECK_EQ_INT(lines_matching(c, image), SSTV_HEIGHT);
    free(c);
    free(image);
}

static void test_clock_offset(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = malloc(sizeof *c);
    sstv_mod m;
    /* Transmitter 300 ppm slow relative to the receiver: 6x a typical crystal error,
     * 0.12 symbol of drift by the end of a line. */
    sstv_mod_init(&m, 44100.0f * (1.0f - 0.0003f), AMP);
    run_image(&m, 44100.0f, 0.0f, c, image);
    int ok = lines_matching(c, image);
    printf("  clock -300ppm: %d/%d lines intact\n", ok, SSTV_HEIGHT);
    CHECK_EQ_INT(ok, SSTV_HEIGHT);
    free(c);
    free(image);
}

static void test_other_sample_rate(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = malloc(sizeof *c);
    sstv_mod m;
    sstv_mod_init(&m, 22050.0f, AMP);
    run_image(&m, 22050.0f, 0.0f, c, image);
    CHECK_EQ_INT(lines_matching(c, image), SSTV_HEIGHT);
    free(c);
    free(image);
}

static void test_lost_sync(void) {
    /* Three lines; the middle one's sync is cut to 2 ms. Expect lines 0 and 2 only. */
    float sr = 44100.0f;
    uint8_t pixels[3][SSTV_LINE_BYTES], frame[SSTV_FRAME_BYTES];
    for(int i = 0; i < 3; i++) fill_random(pixels[i], SSTV_LINE_BYTES);
    int cap = 4 * 40000;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = silence_samples(sr, 20);
    int start[3];
    for(int i = 0; i < 3; i++) {
        sstv_frame_pack((uint8_t)i, pixels[i], frame);
        start[i] = n;
        n += sstv_mod_frame(&m, frame, s + n, cap - n);
    }
    n += silence_samples(sr, 20);
    int cut_ms = SSTV_SYNC_US / 1000 - 2; /* leave 2 ms of the burst */
    memset(s + start[1], 0, sizeof(int16_t) * (size_t)silence_samples(sr, cut_ms));

    capture c = {0};
    sstv_demod d;
    sstv_demod_init(&d, sr, on_line, &c);
    feed_chunked(&d, s, n);
    CHECK(c.seen[0]);
    CHECK(!c.seen[1]);
    CHECK(c.seen[2]);
    CHECK(memcmp(c.got[0], pixels[0], SSTV_LINE_BYTES) == 0);
    CHECK(memcmp(c.got[2], pixels[2], SSTV_LINE_BYTES) == 0);
    free(s);
}

static void test_silence_and_reset(void) {
    sstv_demod d;
    sstv_demod_init(&d, 44100.0f, on_line, NULL);
    int16_t zeros[4410] = {0};
    sstv_demod_feed(&d, zeros, 4410);
    sstv_demod_status_t st;
    sstv_demod_status(&d, &st);
    CHECK_EQ_INT(st.tone, -1);
    CHECK(st.level < 0.01f);
    CHECK_EQ_INT(st.lines, 0);
    d.lines = 5;
    d.crc_errors = 2;
    sstv_demod_reset_counters(&d);
    sstv_demod_status(&d, &st);
    CHECK_EQ_INT(st.lines, 0);
    CHECK_EQ_INT(st.crc_errors, 0);
}

int main(void) {
    test_single_line();
    test_full_image_clean();
    /* sigma relative to the square-wave amplitude: 6 dB and 0 dB SNR. */
    test_full_image_noise((float)AMP / 2.0f, "6 dB", SSTV_HEIGHT * 95 / 100);
    test_full_image_noise((float)AMP, "0 dB", SSTV_HEIGHT * 85 / 100);
    test_frequency_offset();
    test_clock_offset();
    test_other_sample_rate();
    test_lost_sync();
    test_silence_and_reset();
    TEST_MAIN_END();
}
