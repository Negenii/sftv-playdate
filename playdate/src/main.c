#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_frame.h"
#include "mic_ring.h"

#define STATUS_H 20
#define LOST_TIMEOUT_MS 5000

static PlaydateAPI* pd;
static LCDBitmap* image;
static uint8_t* image_data;
static int image_rowbytes;

static sstv_demod demod;
static sstv_rx rx;
static mic_ring ring;

/* Lines arrive decoded and verified (RS + CRC); anything that failed is simply not drawn. */
static uint8_t line_seen[SSTV_HEIGHT];
static int good_lines;
static unsigned last_seg_ms;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static unsigned led_frame_ms; /* virtual LED: last physical frame time */
static unsigned led_block_ms; /* last decoded block time */
static bool show_status = true;

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream, saved as
 * Data/<bundle>/rec.wav for offline analysis of the real channel. */
#define REC_SECONDS 30
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */
static bool rec_saved;

/* ---- audio thread ---- */

static volatile unsigned mic_dropped; /* samples the ring buffer could not take */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    int stored = mic_ring_write(&ring, data, len);
    if(stored < len) mic_dropped += (unsigned)(len - stored);
    return 1;
}

/* ---- main thread ---- */

static void on_line(void* ctx, uint16_t line, const uint8_t* pixels) {
    (void)ctx;
    last_seg_ms = pd->system->getCurrentTimeMilliseconds();
    if(line >= SSTV_HEIGHT) return;
    uint8_t* dst = image_data + line * image_rowbytes;
    /* PBM: 1 = black. Playdate bitmap: 1 = white. */
    for(int i = 0; i < SSTV_LINE_BYTES; i++) dst[i] = (uint8_t)~pixels[i];
    if(!line_seen[line]) good_lines++;
    line_seen[line] = 1;
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    (void)ctx;
    led_frame_ms = pd->system->getCurrentTimeMilliseconds();
    int before = rx.blocks_ok;
    sstv_rx_phys(&rx, frame, soft);
    if(rx.blocks_ok != before) led_block_ms = led_frame_ms;
}

static void clear_image(void) {
    memset(image_data, 0xFF, (size_t)image_rowbytes * SSTV_HEIGHT);
    memset(line_seen, 0, sizeof line_seen);
    good_lines = 0;
    sstv_demod_reset_counters(&demod);
    sstv_rx_reset_counters(&rx);
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
        unsigned now = pd->system->getCurrentTimeMilliseconds();
        bool quiet = good_lines > 0 && now - last_seg_ms > LOST_TIMEOUT_MS;
        static const char tone_names[] = "0123S";
        char tone[2] = {st.tone >= 0 && st.tone <= SSTV_NUM_TONES ? tone_names[st.tone] : '-', 0};
        if(rec_len >= 0) {
            snprintf(text, sizeof text, "REC %d s  lines %d/%d", rec_len / REC_RATE, good_lines, SSTV_HEIGHT);
        } else if(good_lines == SSTV_HEIGHT) {
            snprintf(text, sizeof text, "complete %d/%d  rs fixed %d  A: clear", good_lines, SSTV_HEIGHT,
                     rx.rs_corrected);
        } else if(quiet) {
            snprintf(text, sizeof text, "signal lost  lines %d/%d  bad frames %d%s", good_lines, SSTV_HEIGHT,
                     rx.frames_bad, rec_saved ? "  (rec.wav saved)" : "");
        } else {
            snprintf(text, sizeof text, "lines %d/%d bad %d rs %d fw %d/%d drop %u %s", good_lines,
                     SSTV_HEIGHT, rx.frames_bad, rx.rs_corrected, demod.flywheel_frames, demod.flywheel_left,
                     mic_dropped, tone);
        }
    }
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, 4, y + 3);

    /* virtual LED (the Playdate has no user LED): a dot that lights for 150 ms on every
     * received frame and turns into a ring for 400 ms when a block decodes. */
    {
        unsigned now = pd->system->getCurrentTimeMilliseconds();
        int cx = LCD_COLUMNS - 96, cy = y + 10;
        if(now - led_block_ms < 400) {
            pd->graphics->drawEllipse(cx - 6, cy - 6, 12, 12, 3, 0.0f, 0.0f, kColorBlack);
        } else if(now - led_frame_ms < 150) {
            pd->graphics->fillEllipse(cx - 5, cy - 5, 10, 10, 0.0f, 0.0f, kColorBlack);
        } else {
            pd->graphics->drawEllipse(cx - 5, cy - 5, 10, 10, 1, 0.0f, 0.0f, kColorBlack);
        }
    }

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
    rec_buf = pd->system->realloc(NULL, sizeof(int16_t) * REC_SECONDS * REC_RATE);
    rec_len = -1;
    mic_ring_init(&ring);
    sstv_rx_init(&rx, on_line, NULL);
    sstv_demod_init(&demod, 44100.0f, on_phys, NULL);
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
