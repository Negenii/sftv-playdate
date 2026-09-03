#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "sstv_frame.h"
#include "mic_ring.h"

#define STATUS_H 20
#define LOST_TIMEOUT_MS 5000
#define LINE_MS (SSTV_LINE_US / 1000)

static PlaydateAPI* pd;
static LCDBitmap* image;
static uint8_t* image_data;
static int image_rowbytes;

static sstv_demod demod;
static mic_ring ring;

/* Per-row state. The transmitter loops over the image, so every row gets several chances:
 * a row is "good" once a copy passes Reed-Solomon + CRC; until then soft decisions from
 * each pass are accumulated and re-decoded, so weak receptions add up. */
static float* acc; /* SSTV_HEIGHT * SSTV_FRAME_BITS soft sums */
static uint8_t row_state[SSTV_HEIGHT]; /* 0 none, 1 noisy, 2 good */
static int distinct_lines; /* rows with anything drawn */
static int good_lines; /* rows with a verified copy */
static int accumulated_lines; /* noisy lines merged into the accumulator */
static int ref_line = -1; /* last confidently identified line ... */
static unsigned ref_ms; /* ... and when it arrived: predicts the next line number */
static unsigned last_line_ms;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool show_status = true;

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream, saved as
 * Data/<bundle>/rec.wav for offline analysis of the real channel. */
#define REC_SECONDS 30
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */
static bool rec_saved;

/* ---- audio thread ---- */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    mic_ring_write(&ring, data, len);
    return 1;
}

/* ---- main thread ---- */

static void draw_row(int row, const uint8_t* pixels) {
    uint8_t* dst = image_data + row * image_rowbytes;
    /* PBM: 1 = black. Playdate bitmap: 1 = white. */
    for(int i = 0; i < SSTV_LINE_BYTES; i++) dst[i] = (uint8_t)~pixels[i];
    if(row_state[row] == 0) distinct_lines++;
}

static void mark_good(int row, const uint8_t* pixels) {
    draw_row(row, pixels);
    if(row_state[row] != 2) good_lines++;
    row_state[row] = 2;
    /* Pin the accumulator so later noisy copies cannot flip a verified row. */
    float* a = acc + row * SSTV_FRAME_BITS;
    for(int i = 0; i < SSTV_FRAME_BITS; i++) a[i] = 0.0f;
}

static int hamming8(uint8_t a, uint8_t b) {
    uint8_t x = a ^ b;
    int n = 0;
    while(x) {
        n += x & 1;
        x >>= 1;
    }
    return n;
}

static void on_line(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok, const float* soft, int corrected) {
    (void)ctx;
    (void)corrected;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    last_line_ms = now;

    if(crc_ok) {
        if(line_no < SSTV_HEIGHT) {
            mark_good(line_no, pixels);
            ref_line = line_no;
            ref_ms = now;
        }
        return;
    }

    /* Corrupted line: the line-number byte itself may be wrong. Lines are sent in order
     * every LINE_MS, so predict the number from the last verified line and only accept
     * the decoded byte if it agrees (within 2 bits) with the prediction. */
    int row;
    if(ref_line < 0) {
        if(line_no >= SSTV_HEIGHT) return;
        row = line_no;
    } else {
        unsigned elapsed = now - ref_ms;
        int steps = (int)((elapsed + LINE_MS / 2) / LINE_MS);
        row = (ref_line + steps) % SSTV_HEIGHT;
        if(hamming8((uint8_t)row, line_no) > 2) return;
    }
    if(row_state[row] == 2) return; /* already verified, nothing to add */

    float* a = acc + row * SSTV_FRAME_BITS;
    for(int i = 0; i < SSTV_FRAME_BITS; i++) a[i] += soft[i];
    accumulated_lines++;

    /* Re-decide the row from everything heard so far. */
    uint8_t frame[SSTV_FRAME_BYTES];
    memset(frame, 0, sizeof frame);
    for(int i = 0; i < SSTV_FRAME_BITS; i++) {
        if(a[i] > 0.0f) frame[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
    uint8_t got_line, px[SSTV_LINE_BYTES];
    if(sstv_frame_unpack_ex(frame, &got_line, px, NULL) && got_line == row) {
        mark_good(row, px);
        ref_line = row;
        ref_ms = now;
        return;
    }
    draw_row(row, px);
    row_state[row] = 1;
}

static void clear_image(void) {
    memset(image_data, 0xFF, (size_t)image_rowbytes * SSTV_HEIGHT);
    memset(row_state, 0, sizeof row_state);
    memset(acc, 0, sizeof(float) * SSTV_HEIGHT * SSTV_FRAME_BITS);
    distinct_lines = 0;
    good_lines = 0;
    accumulated_lines = 0;
    ref_line = -1;
    sstv_demod_reset_counters(&demod);
}

static void start_mic(void) {
    pd->sound->setMicCallback(mic_cb, NULL, kMicInputAutodetect);
}

static void mic_access_cb(bool allowed, void* userdata) {
    (void)userdata;
    mic_state = allowed ? 1 : -1;
    if(allowed) start_mic();
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void save_recording(void) {
    SDFile* f = pd->file->open("rec.wav", kFileWrite);
    if(!f) return;
    uint32_t data_bytes = (uint32_t)rec_len * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16);
    h[20] = 1; h[21] = 0; /* PCM */
    h[22] = 1; h[23] = 0; /* mono */
    put_u32(h + 24, REC_RATE);
    put_u32(h + 28, REC_RATE * 2);
    h[32] = 2; h[33] = 0;
    h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
    pd->file->write(f, h, 44);
    pd->file->write(f, rec_buf, (unsigned)data_bytes);
    pd->file->close(f);
}

static void draw_status(void) {
    sstv_demod_status_t st;
    sstv_demod_status(&demod, &st);

    int y = LCD_ROWS - STATUS_H;
    pd->graphics->fillRect(0, y, LCD_COLUMNS, STATUS_H, kColorWhite);
    pd->graphics->drawLine(0, y, LCD_COLUMNS, y, 1, kColorBlack);

    char text[96];
    if(mic_state < 0) {
        snprintf(text, sizeof text, "microphone access denied");
    } else if(mic_state == 0) {
        snprintf(text, sizeof text, "waiting for microphone permission...");
    } else {
        const char* tone = st.tone == SSTV_NUM_TONES ? "sync" : st.tone == 3 ? "3" : st.tone == 2 ? "2" :
                           st.tone == 1 ? "1" : st.tone == 0 ? "0" : "-";
        unsigned now = pd->system->getCurrentTimeMilliseconds();
        bool quiet = distinct_lines > 0 && now - last_line_ms > LOST_TIMEOUT_MS;
        int noisy = distinct_lines - good_lines;
        if(rec_len >= 0) {
            snprintf(text, sizeof text, "REC %d s  good %d/%d", rec_len / REC_RATE, good_lines, SSTV_HEIGHT);
        } else if(rec_saved) {
            snprintf(text, sizeof text, "saved rec.wav  good %d/%d  noisy %d", good_lines, SSTV_HEIGHT,
                     distinct_lines - good_lines);
        } else if(good_lines == SSTV_HEIGHT) {
            snprintf(text, sizeof text, "complete 240/240  rs fixed %d  A: clear", st.rs_corrected);
        } else if(quiet) {
            snprintf(text, sizeof text, "signal lost  good %d/%d  noisy %d", good_lines, SSTV_HEIGHT, noisy);
        } else {
            snprintf(text, sizeof text, "good %d/%d  noisy %d  rs %d  tone %s", good_lines, SSTV_HEIGHT,
                     noisy, st.rs_corrected, tone);
        }
    }
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 4, y + 3);

    /* level bar, right side */
    int bar_w = 80, bar_x = LCD_COLUMNS - bar_w - 4, bar_y = y + 6;
    pd->graphics->drawRect(bar_x, bar_y, bar_w, 8, kColorBlack);
    int fill = (int)(st.level * (float)(bar_w - 2));
    if(fill > 0) pd->graphics->fillRect(bar_x + 1, bar_y + 1, fill, 6, kColorBlack);
}

static int update(void* userdata) {
    (void)userdata;

    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);
    if(pushed & kButtonA) clear_image();
    if(pushed & kButtonB) show_status = !show_status;

    if((pushed & kButtonUp) && rec_len < 0) {
        rec_len = 0;
        rec_saved = false;
    }

    int16_t buf[1024];
    int n;
    while((n = mic_ring_read(&ring, buf, 1024)) > 0) {
        sstv_demod_feed(&demod, buf, n);
        if(rec_len >= 0 && rec_len < REC_SECONDS * REC_RATE) {
            int room = REC_SECONDS * REC_RATE - rec_len;
            int take = n < room ? n : room;
            memcpy(rec_buf + rec_len, buf, sizeof(int16_t) * (size_t)take);
            rec_len += take;
            if(rec_len >= REC_SECONDS * REC_RATE) {
                save_recording();
                rec_len = -1;
                rec_saved = true;
            }
        }
    }

    pd->graphics->clear(kColorWhite);
    pd->graphics->drawBitmap(image, 0, 0, kBitmapUnflipped);
    if(show_status) draw_status();
    return 1;
}

static void init(void) {
    image = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
    int w, h;
    pd->graphics->getBitmapData(image, &w, &h, &image_rowbytes, NULL, &image_data);
    acc = pd->system->realloc(NULL, sizeof(float) * SSTV_HEIGHT * SSTV_FRAME_BITS);
    rec_buf = pd->system->realloc(NULL, sizeof(int16_t) * REC_SECONDS * REC_RATE);
    rec_len = -1;
    mic_ring_init(&ring);
    sstv_demod_init(&demod, 44100.0f, on_line, NULL);
    clear_image();

    enum accessReply r = pd->sound->requestMicAccess(
        "Receive images over sound from a Flipper Zero", mic_access_cb, NULL);
    if(r == kAccessAllow) {
        mic_state = 1;
        start_mic();
    } else if(r == kAccessDeny) {
        mic_state = -1;
    } /* kAccessAsk: wait for mic_access_cb */
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    if(event == kEventInit) {
        pd = playdate;
        pd->display->setRefreshRate(50);
        init();
        pd->system->setUpdateCallback(update, NULL);
    } else if(event == kEventTerminate || event == kEventLock || event == kEventLowPower) {
        /* The audio thread keeps calling the mic callback after the game is unloaded
         * unless it is removed here; the next launch then faults inside freed memory. */
        if(pd) pd->sound->setMicCallback(NULL, NULL, kMicInputAutodetect);
        mic_state = 0;
    } else if(event == kEventUnlock) {
        if(pd && mic_state == 0) {
            mic_state = 1;
            start_mic();
        }
    }
    return 0;
}
