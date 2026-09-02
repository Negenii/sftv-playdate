#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "mic_ring.h"

#define STATUS_H 20
#define COMPLETE_TIMEOUT_MS 3000

static PlaydateAPI* pd;
static LCDBitmap* image;
static uint8_t* image_data;
static int image_rowbytes;

static sstv_demod demod;
static mic_ring ring;

static uint8_t line_seen[SSTV_HEIGHT];
static int distinct_lines;
static unsigned last_line_ms;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool show_status = true;

/* ---- audio thread ---- */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    mic_ring_write(&ring, data, len);
    return 1;
}

/* ---- main thread ---- */

static void on_line(void* ctx, uint8_t line_no, const uint8_t* pixels, bool crc_ok) {
    (void)ctx;
    (void)crc_ok;
    if(line_no >= SSTV_HEIGHT) return;
    uint8_t* row = image_data + line_no * image_rowbytes;
    /* PBM: 1 = black. Playdate bitmap: 1 = white. */
    for(int i = 0; i < SSTV_LINE_BYTES; i++) row[i] = (uint8_t)~pixels[i];
    if(!line_seen[line_no]) {
        line_seen[line_no] = 1;
        distinct_lines++;
    }
    last_line_ms = pd->system->getCurrentTimeMilliseconds();
}

static void clear_image(void) {
    memset(image_data, 0xFF, (size_t)image_rowbytes * SSTV_HEIGHT);
    memset(line_seen, 0, sizeof line_seen);
    distinct_lines = 0;
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
        const char* tone = st.tone == 2 ? "sync" : st.tone == 1 ? "1" : st.tone == 0 ? "0" : "-";
        unsigned now = pd->system->getCurrentTimeMilliseconds();
        bool complete = distinct_lines == SSTV_HEIGHT ||
                        (distinct_lines > 0 && now - last_line_ms > COMPLETE_TIMEOUT_MS);
        if(complete) {
            snprintf(text, sizeof text, "complete  %d/%d lines  crc err %d", distinct_lines,
                     SSTV_HEIGHT, st.crc_errors);
        } else {
            snprintf(text, sizeof text, "lines %d/%d  crc err %d  tone %s", distinct_lines,
                     SSTV_HEIGHT, st.crc_errors, tone);
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

    int16_t buf[1024];
    int n;
    while((n = mic_ring_read(&ring, buf, 1024)) > 0) {
        sstv_demod_feed(&demod, buf, n);
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
    }
    return 0;
}
