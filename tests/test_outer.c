/* Receiver-level tests of the outer code: physical frames go straight into sstv_rx_phys,
 * losses are simulated by garbling (noise with low confidence) or skipping physical frames. */
#include "sstv_frame.h"
#include "sstv_rx.h"
#include "sstv_preview.h"
#include "test_util.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint8_t got[SSTV_HEIGHT][SSTV_LINE_BYTES];
    uint8_t seen[SSTV_HEIGHT];
    int ends;
    int starts;
    int preview_chunks, tiny_chunks;
} capture;

static void on_pixels(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* px) {
    capture* c = ctx;
    memcpy(c->got[line] + x_byte, px, (size_t)nbytes);
    c->seen[line] |= nbytes == SSTV_LINE_BYTES ? 3 : (x_byte == 0 ? 1 : 2);
}
static void on_meta(void* ctx, const char* name, bool is_end, int total, bool coded) {
    (void)name; (void)total; (void)coded;
    if(is_end) ((capture*)ctx)->ends++;
    else ((capture*)ctx)->starts++;
}
static void on_preview(void* ctx, int layer, int chunk, int nbytes, const uint8_t* data) {
    (void)chunk; (void)nbytes; (void)data;
    if(layer == 0) ((capture*)ctx)->tiny_chunks++;
    else if(layer == 1) ((capture*)ctx)->preview_chunks++;
}

static int lines_ok(const capture* c, const uint8_t* image) {
    int ok = 0;
    for(int y = 0; y < SSTV_HEIGHT; y++)
        if(c->seen[y] == 3 && memcmp(c->got[y], image + y * SSTV_LINE_BYTES, SSTV_LINE_BYTES) == 0) ok++;
    return ok;
}

static uint32_t st = 7;
static uint32_t rnd(void) {
    st = st * 1103515245u + 12345u;
    return st >> 8;
}

static bool in(const int* list, int n, int v) {
    for(int i = 0; i < n; i++) if(list[i] == v) return true;
    return false;
}

/* Feeds a pass; physical frames in `garble` arrive as noise with low confidence, frames in
 * `skip` never arrive. */
static void feed(sstv_rx* rx, const uint8_t* phys, int frames, const int* garble, int n_garble, const int* skip, int n_skip) {
    float soft_good[SSTV_FRAME_BITS], soft_bad[SSTV_FRAME_BITS];
    for(int i = 0; i < SSTV_FRAME_BITS; i++) {
        soft_good[i] = 1.0f;
        soft_bad[i] = 0.05f;
    }
    for(int f = 0; f < frames; f++) {
        if(in(skip, n_skip, f)) continue;
        if(in(garble, n_garble, f)) {
            uint8_t noise[SSTV_FRAME_BYTES];
            for(int i = 0; i < SSTV_FRAME_BYTES; i++) noise[i] = (uint8_t)rnd();
            sstv_rx_phys(rx, noise, soft_bad);
        } else {
            sstv_rx_phys(rx, phys + f * SSTV_FRAME_BYTES, soft_good);
        }
    }
}

static void run_case(const char* label, const uint8_t* image, const uint8_t* phys, int frames, const int* garble, int n_garble, const int* skip, int n_skip, int want_lines, int want_repaired_min) {
    capture* c = calloc(1, sizeof *c);
    sstv_rx rx;
    sstv_rx_init(&rx, on_pixels, on_meta, on_preview, c);
    feed(&rx, phys, frames, garble, n_garble, skip, n_skip);
    int ok = lines_ok(c, image);
    printf("  %s: %d/%d lines, frames ok %d bad %d, repaired %d frames in %d groups, %d failed\n", label, ok, SSTV_HEIGHT,
           rx.frames_ok, rx.frames_bad, rx.frames_repaired, rx.groups_repaired, rx.repairs_failed);
    CHECK_EQ_INT(ok, want_lines);
    CHECK(rx.frames_repaired >= want_repaired_min);
    CHECK_EQ_INT(c->ends, 1);
    CHECK_EQ_INT(c->starts, 1);
    if(want_lines == SSTV_HEIGHT) {
        CHECK_EQ_INT(c->tiny_chunks, SSTV_TINY_CHUNKS);
        CHECK_EQ_INT(c->preview_chunks, SSTV_PREVIEW_CHUNKS);
    }
    free(c);
}

int main(void) {
    uint8_t* image = malloc(SSTV_IMAGE_BYTES);
    uint8_t* phys = malloc(SSTV_MAX_FRAMES * SSTV_FRAME_BYTES);
    bool coded;

    /* raw pass (random image) */
    for(int i = 0; i < SSTV_IMAGE_BYTES; i++) image[i] = (uint8_t)rnd();
    int frames = sstv_build_physical(image, "noise", phys, NULL, &coded);
    CHECK(!coded);
    printf("raw pass: %d frames\n", frames);
    run_case("raw clean", image, phys, frames, NULL, 0, NULL, 0, SSTV_HEIGHT, 0);
    /* Physical layout: group 0 = the 28 intro frames + SSTV_OUTER_PARITY parity, then groups of
     * 108 picture frames + parity. G1 = first physical frame of the first picture group; a
     * frame index in picture group k (k >= 1) is G1 + (k - 1) * (108 + parity) + offset. */
    const int P = SSTV_OUTER_PARITY, G1 = 28 + P, GS = 108 + P;
    /* two garbled physical frames in one block kill its 4 logical frames; four such blocks in one
       group = 16 lost frames: all repaired */
    int g16[] = {G1, G1 + 1, G1 + 20, G1 + 21, G1 + 40, G1 + 41, G1 + 60, G1 + 61};
    run_case("raw 16 lost in a group", image, phys, frames, g16, 8, NULL, 0, SSTV_HEIGHT, 16);
    /* a fifth block lost in the same group: 20 > 16, the group stays short (10 lines), later
       groups fine */
    int g20[] = {G1, G1 + 1, G1 + 20, G1 + 21, G1 + 40, G1 + 41, G1 + 60, G1 + 61, G1 + 80, G1 + 81};
    run_case("raw 20 lost in a group", image, phys, frames, g20, 10, NULL, 0, SSTV_HEIGHT - 10, 0);
    /* losses spread over groups, including a parity block (physical 108..119 are group 0's
       parity) and the tiny preview block (4..7) */
    int spread[] = {4, 5, G1 + 70, G1 + 71, G1 + 90, G1 + 91, G1 + GS + 90, G1 + GS + 91, G1 + 3 * GS + 0, G1 + 3 * GS + 1};
    run_case("raw spread losses", image, phys, frames, spread, 10, NULL, 0, SSTV_HEIGHT, 12);
    /* physical frames that never arrive (sync lost): one per group */
    int skip[] = {G1 + 10, G1 + GS + 40, G1 + 2 * GS + 50, G1 + 3 * GS + 50};
    run_case("raw skipped frames", image, phys, frames, NULL, 0, skip, 4, SSTV_HEIGHT, 8);
    /* the START block itself lost: the pass still opens on the repaired... no: without START
       the receiver cannot announce; it decodes the rest and END announces the name */
    /* START lost together with data blocks in the first and the last (short) group: the
       first group repairs on its parity (full group assumed), the last one on END */
    int gstart[] = {0, 1, G1, G1 + 1, G1 + 3 * GS + 100, G1 + 3 * GS + 101};
    {
        capture* c = calloc(1, sizeof *c);
        sstv_rx rx;
        sstv_rx_init(&rx, on_pixels, on_meta, on_preview, c);
        feed(&rx, phys, frames, gstart, 6, NULL, 0);
        printf("  raw START + blocks lost: %d/%d lines, repaired %d frames in %d groups\n", lines_ok(c, image), SSTV_HEIGHT, rx.frames_repaired, rx.groups_repaired);
        CHECK_EQ_INT(lines_ok(c, image), SSTV_HEIGHT);
        CHECK_EQ_INT(c->ends, 1);
        CHECK_EQ_INT(c->starts, 0);
        CHECK_EQ_INT(rx.frames_repaired, 12);
        free(c);
    }

    /* coded pass (8-line stripes compress well) */
    for(int y = 0; y < SSTV_HEIGHT; y++)
        for(int i = 0; i < SSTV_LINE_BYTES; i++) image[y * SSTV_LINE_BYTES + i] = (y / 8) & 1 ? 0xF0 : 0x0F;
    frames = sstv_build_physical(image, "stripes", phys, NULL, &coded);
    CHECK(coded);
    printf("coded pass: %d frames\n", frames);
    run_case("coded clean", image, phys, frames, NULL, 0, NULL, 0, SSTV_HEIGHT, 0);
    /* one garbled frame of a coded block: the inner RS with erasures fixes it */
    const int C1 = 32 + SSTV_OUTER_PARITY; /* coded: intro 32 frames + parity, then group 1 */
    int c1[] = {C1 + 1};
    run_case("coded one garbled frame", image, phys, frames, c1, 1, NULL, 0, SSTV_HEIGHT, 0);
    /* a whole coded block lost (two garbled frames): its lines come back after the parity */
    int c2[] = {C1, C1 + 1};
    run_case("coded block lost", image, phys, frames, c2, 2, NULL, 0, SSTV_HEIGHT, 4);
    /* the schedule block (intro group, physical 28-31) and the first picture block (group 1,
       physical C1..C1+3) both lost: repaired by their own group parity */
    int c3[] = {28, 29, C1, C1 + 1};
    run_case("coded schedule + block lost", image, phys, frames, c3, 4, NULL, 0, SSTV_HEIGHT, 8);
    /* the user's case: a tiny block (physical 4-7) and a preview block (physical 12-15) lost;
       the intro group's parity (sent right after the intro) rebuilds them, so tiny 4/4 and
       preview 20/20 still arrive -- and would arrive while the scene is still up */
    int cp[] = {4, 5, 12, 13};
    run_case("coded intro previews lost", image, phys, frames, cp, 4, NULL, 0, SSTV_HEIGHT, 8);

    free(image);
    free(phys);
    TEST_MAIN_END();
}
