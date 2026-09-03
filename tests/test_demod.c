#include "test_util.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_preview.h"
#include "sstv_modem_ref.h"
#include "sstv_frame.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AMP 8000

/* demod -> rx -> segments */
typedef struct {
    sstv_rx rx;
    int count; /* lines delivered */
    uint8_t got[SSTV_HEIGHT][SSTV_LINE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
} capture;

static void on_pixels(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels) {
    capture* c = ctx;
    c->count++;
    if(line < SSTV_HEIGHT && x_byte + nbytes <= SSTV_LINE_BYTES) {
        memcpy(c->got[line] + x_byte, pixels, (size_t)nbytes);
        c->seen[line] |= (uint8_t)(x_byte == 0 ? 1 : 2);
        if(nbytes == SSTV_LINE_BYTES) c->seen[line] = 3;
    }
}

static char last_name[SSTV_PAYLOAD_BYTES + 1];
static int end_seen;
static int last_total;
static bool last_coded;
static void on_meta(void* ctx, const char* name, bool is_end, int total_frames, bool coded) {
    (void)ctx;
    strncpy(last_name, name, SSTV_PAYLOAD_BYTES);
    if(is_end) end_seen++;
    else { last_total = total_frames; last_coded = coded; }
}

static uint8_t preview_got[SSTV_PREVIEW_BYTES];
static uint8_t tiny_got[SSTV_TINY_BYTES];
static int preview_chunks, tiny_chunks;
static void on_preview(void* ctx, int layer, int chunk, int nbytes, const uint8_t* data) {
    (void)ctx;
    if(layer == 0) {
        memcpy(tiny_got + chunk * SSTV_PREVIEW_CHUNK, data, (size_t)nbytes);
        tiny_chunks++;
    } else {
        memcpy(preview_got + chunk * SSTV_PREVIEW_CHUNK, data, (size_t)nbytes);
        preview_chunks++;
    }
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    capture* c = ctx;
    sstv_rx_phys(&c->rx, frame, soft);
}

/* Removes the preview blocks so the first data block directly follows the START block
 * (tests that send only the first few frames). */
static void drop_preview(uint8_t* phys, int frames) {
    memmove(phys + SSTV_ILEAVE * SSTV_FRAME_BYTES, phys + (SSTV_ILEAVE + SSTV_INTRO_FRAMES) * SSTV_FRAME_BYTES,
            (size_t)(frames - SSTV_ILEAVE - SSTV_INTRO_FRAMES) * SSTV_FRAME_BYTES);
}

static capture* capture_new(void) {
    capture* c = calloc(1, sizeof *c);
    sstv_rx_init(&c->rx, on_pixels, on_meta, on_preview, c);
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

static int lines_matching(const capture* c, const uint8_t* image) {
    int ok = 0;
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        if(c->seen[y] == 3 && memcmp(c->got[y], image + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0) ok++;
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
    return lines_matching(c, image);
}

static void test_single_block(void) {
    /* One interleave block of random segments, preceded by a short leader. */
    float sr = 44100.0f;
    uint8_t* image = calloc(SSTV_IMAGE_BYTES, 1);
    fill_random(image, SSTV_IMAGE_BYTES);
    uint8_t* phys = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    drop_preview(phys, sstv_build_physical(image, "t", phys, NULL, NULL));
    int cap = silence_samples(sr, 200) + 2 * SSTV_ILEAVE * 30000;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = silence_samples(sr, 20);
    n += sstv_mod_tone(&m, m.tones[1], 50000, s + n, cap - n);
    /* meta block + first data block (raw mode: 4 half-line segments = lines 0,1) */
    for(int p = 0; p < 2 * SSTV_ILEAVE; p++) n += sstv_mod_frame(&m, phys + p * SSTV_FRAME_BYTES, s + n, cap - n);
    n += silence_samples(sr, 20);

    capture* c = capture_new();
    last_name[0] = 0;
    sstv_demod d;
    sstv_demod_init(&d, sr, on_phys, c);
    feed_chunked(&d, s, n);
    CHECK_EQ_INT(d.frames, 2 * SSTV_ILEAVE);
    CHECK(strcmp(last_name, "t") == 0);
    CHECK_EQ_INT(c->count, SSTV_ILEAVE); /* 4 segments */
    CHECK_EQ_INT(lines_matching(c, image), 2);
    /* the flywheel may already be waiting for a next frame; it must not have produced one */
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
    end_seen = 0;
    int ok = run_image(&m, 44100.0f, NULL, c, image);
    CHECK_EQ_INT(c->count, SSTV_NUM_SEGS); /* random image goes raw: one callback per half line */
    CHECK_EQ_INT(ok, SSTV_HEIGHT);
    CHECK_EQ_INT(end_seen, 1);
    CHECK(strcmp(last_name, "image") == 0);
    CHECK_EQ_INT(last_total, SSTV_NUM_SEGS + 2 * SSTV_ILEAVE + SSTV_INTRO_FRAMES); /* raw pass length announced */
    CHECK(!last_coded);
    /* a compressible image needs fewer frames and still arrives intact */
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        memset(image + y * SSTV_LINE_BYTES, 0, SSTV_LINE_BYTES);
        if(y % 3 == 0) memset(image + y * SSTV_LINE_BYTES + 10, 0xFF, 20);
    }
    capture* c2 = capture_new();
    float sr = 44100.0f;
    uint8_t* phys = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    bool coded = false;
    int frames = sstv_build_physical(image, "shapes", phys, NULL, &coded);
    printf("  compressible image: %d physical frames (raw would be %d)\n", frames, SSTV_NUM_SEGS + SSTV_ILEAVE);
    CHECK(coded);
    CHECK(frames < SSTV_HEIGHT);
    int cap = sstv_mod_image_capacity(sr);
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m2;
    sstv_mod_init(&m2, sr, AMP);
    int n = silence_samples(sr, 50) + sstv_mod_image(&m2, image, s + silence_samples(sr, 50), cap - 5000);
    sstv_demod d;
    sstv_demod_init(&d, sr, on_phys, c2);
    feed_chunked(&d, s, n);
    CHECK_EQ_INT(lines_matching(c2, image), SSTV_HEIGHT);
    free(s);
    free(phys);
    free(c2);
    free(c);
    free(image);
}

static void test_noise(const noise_spec* ns, const char* label, int min_ok) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 44100.0f, AMP);
    int ok = run_image(&m, 44100.0f, ns, c, image);
    printf("  %s: %d/%d lines intact, %d delivered, rs fixed %d bytes\n", label, ok, SSTV_HEIGHT, c->count,
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
    preview_chunks = tiny_chunks = 0;
    memset(preview_got, 0, sizeof preview_got);
    memset(tiny_got, 0, sizeof tiny_got);
    CHECK_EQ_INT(run_image(&m, 44100.0f, NULL, c, image), SSTV_HEIGHT);
    /* the preview arrived whole and matches what the transmitter computed */
    CHECK_EQ_INT(preview_chunks, SSTV_PREVIEW_CHUNKS);
    CHECK_EQ_INT(tiny_chunks, SSTV_TINY_CHUNKS);
    uint8_t pv[SSTV_PREVIEW_BYTES], tn[SSTV_TINY_BYTES];
    sstv_preview_make(image, pv);
    sstv_preview_make_tiny(image, tn);
    CHECK(memcmp(pv, preview_got, SSTV_PREVIEW_BYTES) == 0);
    CHECK(memcmp(tn, tiny_got, SSTV_TINY_BYTES) == 0);
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
        printf("  clock %+d ppm: %d/%d lines intact\n", sign * 1500, ok, SSTV_HEIGHT);
        CHECK(ok >= SSTV_HEIGHT * 95 / 100); /* 1500 ppm is 1.5x the measured Flipper/Playdate offset */
        free(c);
    }
    free(image);
}

static void test_other_sample_rate(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    capture* c = capture_new();
    sstv_mod m;
    sstv_mod_init(&m, 22050.0f, AMP);
    CHECK_EQ_INT(run_image(&m, 22050.0f, NULL, c, image), SSTV_HEIGHT);
    free(c);
    free(image);
}

static void test_lost_sync(void) {
    /* Two blocks; the second physical frame of block 0 loses most of its sync. The
     * flywheel must start that frame from the predicted edge, so both blocks survive. */
    float sr = 44100.0f;
    uint8_t* image = calloc(SSTV_IMAGE_BYTES, 1);
    fill_random(image, SSTV_IMAGE_BYTES);
    uint8_t* phys = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    drop_preview(phys, sstv_build_physical(image, "t", phys, NULL, NULL));
    int cap = 3 * SSTV_ILEAVE * 30000 + 20000;
    int16_t* s = calloc((size_t)cap, sizeof(int16_t));
    sstv_mod m;
    sstv_mod_init(&m, sr, AMP);
    int n = silence_samples(sr, 20);
    /* meta block, then two raw blocks (lines 0-1 and 2-3); frame 1 of block 1 loses its sync */
    int start[3 * SSTV_ILEAVE];
    for(int p = 0; p < 3 * SSTV_ILEAVE; p++) {
        start[p] = n;
        n += sstv_mod_frame(&m, phys + p * SSTV_FRAME_BYTES, s + n, cap - n);
    }
    n += silence_samples(sr, 20);
    int cut_ms = SSTV_SYNC_US / 1000 - 2;
    memset(s + start[SSTV_ILEAVE + 1], 0, sizeof(int16_t) * (size_t)silence_samples(sr, cut_ms));

    capture* c = capture_new();
    sstv_demod d;
    sstv_demod_init(&d, sr, on_phys, c);
    feed_chunked(&d, s, n);
    CHECK_EQ_INT(lines_matching(c, image), 4);
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
    test_noise(&white6, "white 6 dB", SSTV_HEIGHT);
    test_noise(&white0, "white 0 dB", SSTV_HEIGHT * 92 / 100);
    test_noise(&bursts, "150 ms bursts / 2 s", SSTV_HEIGHT * 97 / 100);
    test_noise(&long_bursts, "400 ms bursts / 3 s", SSTV_HEIGHT * 80 / 100);
    noise_spec word_bursts = {(float)AMP / 4.0f, (float)AMP * 6.0f, 700, 3000};
    test_noise(&word_bursts, "700 ms bursts / 3 s", SSTV_HEIGHT * 50 / 100);
    test_frequency_offset();
    test_clock_offset();
    test_other_sample_rate();
    test_lost_sync();
    test_silence_and_reset();
    TEST_MAIN_END();
}
