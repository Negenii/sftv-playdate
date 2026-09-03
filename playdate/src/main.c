#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_frame.h"
#include "mic_ring.h"

#define LOST_TIMEOUT_MS 5000
#define PANEL_W 150
#define MAX_GALLERY 64

static PlaydateAPI* pd;
static LCDBitmap* image;
static uint8_t* image_data;
static int image_rowbytes;

static sstv_demod demod;
static sstv_rx rx;
static mic_ring ring;

/* Receive state. Half-line segments (raw mode) or whole lines (coded mode) arrive verified. */
static uint8_t seg_seen[SSTV_HEIGHT]; /* bit 0: left half, bit 1: right half */
static int segs_done; /* of SSTV_NUM_SEGS */
static unsigned last_data_ms;
static char current_name[SSTV_PAYLOAD_BYTES + 8] = "untitled.pbm";
static bool image_dirty; /* something received since the last save */
static bool image_saved;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool show_panel; /* side panel with statistics, toggled with Right */
static unsigned led_frame_ms; /* virtual LED: last physical frame time */
static unsigned led_block_ms; /* last decoded block time */
static volatile unsigned mic_dropped;

/* Gallery of saved images. */
static bool in_gallery;
static char gallery_names[MAX_GALLERY][40];
static int gallery_count;
static int gallery_index;
static LCDBitmap* gallery_bitmap;

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream (Data/rec.wav). */
#define REC_SECONDS 30
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */
static bool rec_saved;

/* ---- audio thread ---- */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    int stored = mic_ring_write(&ring, data, len);
    if(stored < len) mic_dropped += (unsigned)(len - stored);
    return 1;
}

/* ---- saving and gallery ---- */

static void sanitize_name(char* out, size_t cap, const char* in) {
    size_t n = 0;
    for(const char* p = in; *p && n + 1 < cap; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        out[n++] = ok ? c : '_';
    }
    out[n] = 0;
    if(n == 0) strncpy(out, "untitled", cap);
    if(!strstr(out, ".pbm") && n + 5 < cap) strcat(out, ".pbm");
}

/* Saves the received bitmap as a P4 PBM under Data/. Same name overwrites. */
static void save_image(void) {
    if(!image_dirty) return;
    char fname[64];
    sanitize_name(fname, sizeof fname, current_name);
    SDFile* f = pd->file->open(fname, kFileWrite);
    if(!f) return;
    char hdr[32];
    int hl = snprintf(hdr, sizeof hdr, "P4\n%d %d\n", SSTV_WIDTH, SSTV_HEIGHT);
    pd->file->write(f, hdr, (unsigned)hl);
    uint8_t row[SSTV_LINE_BYTES];
    for(int y = 0; y < SSTV_HEIGHT; y++) {
        for(int i = 0; i < SSTV_LINE_BYTES; i++) row[i] = (uint8_t)~image_data[y * image_rowbytes + i];
        pd->file->write(f, row, SSTV_LINE_BYTES);
    }
    pd->file->close(f);
    image_dirty = false;
    image_saved = true;
}

static void gallery_list_cb(const char* path, void* userdata) {
    (void)userdata;
    size_t n = strlen(path);
    if(n < 5 || strcmp(path + n - 4, ".pbm") != 0 || gallery_count >= MAX_GALLERY) return;
    strncpy(gallery_names[gallery_count], path, sizeof(gallery_names[0]) - 1);
    gallery_names[gallery_count][sizeof(gallery_names[0]) - 1] = 0;
    gallery_count++;
}

static bool gallery_load(int idx) {
    if(idx < 0 || idx >= gallery_count) return false;
    SDFile* f = pd->file->open(gallery_names[idx], kFileReadData);
    if(!f) return false;
    uint8_t* buf = pd->system->realloc(NULL, SSTV_IMAGE_BYTES + 64);
    int n = pd->file->read(f, buf, SSTV_IMAGE_BYTES + 64);
    pd->file->close(f);
    if(n < SSTV_IMAGE_BYTES) {
        pd->system->realloc(buf, 0);
        return false;
    }
    const uint8_t* px = buf + n - SSTV_IMAGE_BYTES;
    if(!gallery_bitmap) gallery_bitmap = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
    int w, h, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(gallery_bitmap, &w, &h, &rb, NULL, &data);
    for(int y = 0; y < SSTV_HEIGHT; y++)
        for(int i = 0; i < SSTV_LINE_BYTES; i++) data[y * rb + i] = (uint8_t)~px[y * SSTV_LINE_BYTES + i];
    pd->system->realloc(buf, 0);
    return true;
}

static void gallery_open(void) {
    save_image();
    gallery_count = 0;
    pd->file->listfiles(".", gallery_list_cb, NULL, 0);
    in_gallery = true;
    gallery_index = 0;
    gallery_load(gallery_index);
}

/* ---- receive callbacks (main thread) ---- */

static void on_pixels(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels) {
    (void)ctx;
    last_data_ms = pd->system->getCurrentTimeMilliseconds();
    if(line >= SSTV_HEIGHT || x_byte + nbytes > SSTV_LINE_BYTES) return;
    uint8_t* dst = image_data + line * image_rowbytes + x_byte;
    /* PBM: 1 = black. Playdate bitmap: 1 = white. */
    for(int i = 0; i < nbytes; i++) dst[i] = (uint8_t)~pixels[i];
    uint8_t mask = nbytes == SSTV_LINE_BYTES ? 3 : (x_byte == 0 ? 1 : 2);
    uint8_t before = seg_seen[line];
    seg_seen[line] |= mask;
    segs_done += ((seg_seen[line] & 1) - (before & 1)) + (((seg_seen[line] >> 1) & 1) - ((before >> 1) & 1));
    image_dirty = true;
}

static void clear_image(void) {
    memset(image_data, 0xFF, (size_t)image_rowbytes * SSTV_HEIGHT);
    memset(seg_seen, 0, sizeof seg_seen);
    segs_done = 0;
    image_dirty = false;
    image_saved = false;
    sstv_demod_reset_counters(&demod);
    sstv_rx_reset_counters(&rx);
}

static void on_meta(void* ctx, const char* name) {
    (void)ctx;
    /* A new transmission starts: keep what the previous one produced, then start fresh. */
    save_image();
    if(segs_done > 0) clear_image();
    strncpy(current_name, name, SSTV_PAYLOAD_BYTES);
    current_name[SSTV_PAYLOAD_BYTES] = 0;
    last_data_ms = pd->system->getCurrentTimeMilliseconds();
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    (void)ctx;
    led_frame_ms = pd->system->getCurrentTimeMilliseconds();
    int before = rx.blocks_ok;
    sstv_rx_phys(&rx, frame, soft);
    if(rx.blocks_ok != before) led_block_ms = led_frame_ms;
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

/* ---- drawing ---- */

static void draw_led(void) {
    /* Virtual LED in the top-right corner: filled on every received frame, ring when a block
     * decodes, outline otherwise. Always visible. */
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    int cx = LCD_COLUMNS - 12, cy = 12;
    pd->graphics->fillEllipse(cx - 8, cy - 8, 16, 16, 0.0f, 0.0f, kColorWhite);
    if(now - led_block_ms < 400) {
        pd->graphics->drawEllipse(cx - 7, cy - 7, 14, 14, 3, 0.0f, 0.0f, kColorBlack);
    } else if(now - led_frame_ms < 150) {
        pd->graphics->fillEllipse(cx - 6, cy - 6, 12, 12, 0.0f, 0.0f, kColorBlack);
    } else {
        pd->graphics->drawEllipse(cx - 6, cy - 6, 12, 12, 1, 0.0f, 0.0f, kColorBlack);
    }
}

static void draw_text(const char* text, int x, int y) {
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, x, y);
}

static void draw_panel(void) {
    sstv_demod_status_t st;
    sstv_demod_status(&demod, &st);
    int x0 = LCD_COLUMNS - PANEL_W;
    pd->graphics->fillRect(x0, 0, PANEL_W, LCD_ROWS, kColorWhite);
    pd->graphics->drawLine(x0, 0, x0, LCD_ROWS, 1, kColorBlack);
    char t[64];
    int y = 6, dy = 18, x = x0 + 8;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    bool quiet = segs_done > 0 && now - last_data_ms > LOST_TIMEOUT_MS;
    if(mic_state < 0) {
        draw_text("mic denied", x, y);
        return;
    }
    if(mic_state == 0) {
        draw_text("mic permission?", x, y);
        return;
    }
    snprintf(t, sizeof t, "%s", current_name); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "%s", segs_done == SSTV_NUM_SEGS ? "complete" : quiet ? "signal lost" : rx.coded ? "receiving (coded)" : "receiving"); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "lines %d/%d", segs_done / 2, SSTV_HEIGHT); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "frames ok %d bad %d", rx.frames_ok, rx.frames_bad); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "rs fixed %d", rx.rs_corrected); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "flywheel %d", demod.flywheel_frames); draw_text(t, x, y); y += dy;
    snprintf(t, sizeof t, "dropped %u", mic_dropped); draw_text(t, x, y); y += dy;
    static const char tone_names[] = "0123S";
    snprintf(t, sizeof t, "tone %c", st.tone >= 0 && st.tone <= SSTV_NUM_TONES ? tone_names[st.tone] : '-'); draw_text(t, x, y); y += dy;
    if(rec_len >= 0) { snprintf(t, sizeof t, "REC %d s", rec_len / REC_RATE); draw_text(t, x, y); y += dy; }
    else if(rec_saved) { draw_text("rec.wav saved", x, y); y += dy; }
    if(image_saved) { draw_text("saved", x, y); y += dy; }
    /* level bar */
    int bar_w = PANEL_W - 16, bar_x = x, bar_y = LCD_ROWS - 40;
    pd->graphics->drawRect(bar_x, bar_y, bar_w, 8, kColorBlack);
    int fill = (int)(st.level * (float)(bar_w - 2));
    if(fill > 0) pd->graphics->fillRect(bar_x + 1, bar_y + 1, fill, 6, kColorBlack);
    draw_text("A clear  B gallery", x, LCD_ROWS - 24);
}

static void draw_gallery(void) {
    pd->graphics->clear(kColorWhite);
    if(gallery_count == 0) {
        draw_text("gallery is empty", 140, 110);
        draw_text("B back", 160, 140);
        return;
    }
    if(gallery_bitmap) pd->graphics->drawBitmap(gallery_bitmap, 0, 0, kBitmapUnflipped);
    char t[80];
    snprintf(t, sizeof t, "%d/%d  %s", gallery_index + 1, gallery_count, gallery_names[gallery_index]);
    pd->graphics->fillRect(0, LCD_ROWS - 20, LCD_COLUMNS, 20, kColorWhite);
    pd->graphics->drawLine(0, LCD_ROWS - 20, LCD_COLUMNS, LCD_ROWS - 20, 1, kColorBlack);
    draw_text(t, 4, LCD_ROWS - 17);
    draw_text("< >  B back", LCD_COLUMNS - 100, LCD_ROWS - 17);
}

/* ---- main loop ---- */

static int update(void* userdata) {
    (void)userdata;

    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);

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

    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(image_dirty && ((segs_done == SSTV_NUM_SEGS) || (now - last_data_ms > LOST_TIMEOUT_MS))) save_image();

    if(in_gallery) {
        if(pushed & kButtonB) in_gallery = false;
        if((pushed & kButtonRight) && gallery_count > 0) { gallery_index = (gallery_index + 1) % gallery_count; gallery_load(gallery_index); }
        if((pushed & kButtonLeft) && gallery_count > 0) { gallery_index = (gallery_index + gallery_count - 1) % gallery_count; gallery_load(gallery_index); }
        draw_gallery();
        return 1;
    }

    if(pushed & kButtonA) clear_image();
    if(pushed & kButtonB) gallery_open();
    if(pushed & kButtonRight) show_panel = !show_panel;
    if((pushed & kButtonUp) && rec_len < 0) {
        rec_len = 0;
        rec_saved = false;
    }

    pd->graphics->clear(kColorWhite);
    pd->graphics->drawBitmap(image, 0, 0, kBitmapUnflipped);
    if(show_panel) draw_panel();
    draw_led();
    return 1;
}

static void init(void) {
    image = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
    int w, h;
    pd->graphics->getBitmapData(image, &w, &h, &image_rowbytes, NULL, &image_data);
    rec_buf = pd->system->realloc(NULL, sizeof(int16_t) * REC_SECONDS * REC_RATE);
    rec_len = -1;
    mic_ring_init(&ring);
    sstv_rx_init(&rx, on_pixels, on_meta, NULL);
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
        if(pd) {
            save_image();
            pd->sound->setMicCallback(NULL, NULL, kMicInputAutodetect);
        }
        mic_state = 0;
    } else if(event == kEventUnlock) {
        if(pd && mic_state == 0) {
            mic_state = 1;
            start_mic();
        }
    }
    return 0;
}
