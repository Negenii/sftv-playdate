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
static bool finished; /* end-of-transmission meta received */
static unsigned finished_ms;
static int finished_lines;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool show_panel; /* side panel with statistics, toggled with Right */
static unsigned led_frame_ms; /* virtual LED: last physical frame time */
static unsigned led_block_ms; /* last decoded block time */
static volatile unsigned mic_dropped;

/* Gallery of saved images: a grid of thumbnails, A opens one full screen. */
typedef enum { ModeReceive, ModeGrid, ModeFull } UiMode;
static UiMode ui_mode = ModeReceive;
static char gallery_names[MAX_GALLERY][40];
static int gallery_count;
static int gallery_index; /* cursor */
static int gallery_page; /* first tile shown */
static LCDBitmap* gallery_bitmap;
static LCDBitmap* thumbs[MAX_GALLERY]; /* 100x60 thumbnails, loaded lazily */
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define THUMB_W 100
#define THUMB_H 60

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

/* Reads a saved PBM into `px` (SSTV_IMAGE_BYTES). */
static bool read_pbm(const char* name, uint8_t* px) {
    SDFile* f = pd->file->open(name, kFileReadData);
    if(!f) return false;
    uint8_t* buf = pd->system->realloc(NULL, SSTV_IMAGE_BYTES + 64);
    int n = pd->file->read(f, buf, SSTV_IMAGE_BYTES + 64);
    pd->file->close(f);
    bool ok = n >= SSTV_IMAGE_BYTES;
    if(ok) memcpy(px, buf + n - SSTV_IMAGE_BYTES, SSTV_IMAGE_BYTES);
    pd->system->realloc(buf, 0);
    return ok;
}

static void pbm_to_bitmap(const uint8_t* px, LCDBitmap* bmp) {
    int w, h, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(bmp, &w, &h, &rb, NULL, &data);
    for(int y = 0; y < SSTV_HEIGHT; y++)
        for(int i = 0; i < SSTV_LINE_BYTES; i++) data[y * rb + i] = (uint8_t)~px[y * SSTV_LINE_BYTES + i];
}

/* 4:1 nearest-neighbour thumbnail (400x240 -> 100x60). */
static LCDBitmap* make_thumb(const uint8_t* px) {
    LCDBitmap* t = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorWhite);
    int w, h, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(t, &w, &h, &rb, NULL, &data);
    for(int y = 0; y < THUMB_H; y++) {
        for(int x = 0; x < THUMB_W; x++) {
            int sx = x * 4 + 1, sy = y * 4 + 1;
            int black = (px[sy * SSTV_LINE_BYTES + (sx >> 3)] >> (7 - (sx & 7))) & 1;
            if(black) data[y * rb + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
    return t;
}

static bool gallery_load_full(int idx) {
    if(idx < 0 || idx >= gallery_count) return false;
    uint8_t* px = pd->system->realloc(NULL, SSTV_IMAGE_BYTES);
    bool ok = read_pbm(gallery_names[idx], px);
    if(ok) {
        if(!gallery_bitmap) gallery_bitmap = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
        pbm_to_bitmap(px, gallery_bitmap);
    }
    pd->system->realloc(px, 0);
    return ok;
}

static void gallery_ensure_thumbs(int first, int count) {
    uint8_t* px = pd->system->realloc(NULL, SSTV_IMAGE_BYTES);
    for(int i = first; i < first + count && i < gallery_count; i++) {
        if(thumbs[i]) continue;
        if(read_pbm(gallery_names[i], px)) thumbs[i] = make_thumb(px);
    }
    pd->system->realloc(px, 0);
}

static void gallery_open(void) {
    save_image();
    for(int i = 0; i < gallery_count; i++) {
        if(thumbs[i]) {
            pd->graphics->freeBitmap(thumbs[i]);
            thumbs[i] = NULL;
        }
    }
    gallery_count = 0;
    pd->file->listfiles(".", gallery_list_cb, NULL, 0);
    ui_mode = ModeGrid;
    gallery_index = 0;
    gallery_page = 0;
    gallery_ensure_thumbs(0, GRID_PER_PAGE);
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
    finished = false;
    sstv_demod_reset_counters(&demod);
    sstv_rx_reset_counters(&rx);
}

static void on_meta(void* ctx, const char* name, bool is_end) {
    (void)ctx;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(is_end) {
        /* The transmitter is done: save and announce. */
        finished = true;
        finished_ms = now;
        finished_lines = segs_done / 2;
        save_image();
        return;
    }
    /* A new transmission starts: keep what the previous one produced, then start fresh. */
    save_image();
    if(segs_done > 0) clear_image();
    finished = false;
    strncpy(current_name, name, SSTV_PAYLOAD_BYTES);
    current_name[SSTV_PAYLOAD_BYTES] = 0;
    last_data_ms = now;
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

static int text_width(const char* text) {
    return pd->graphics->getTextWidth(NULL, text, strlen(text), kASCIIEncoding, 0);
}

/* Rounded label; filled = black pill with white text. */
static int draw_pill(const char* text, int x, int y, bool filled) {
    int w = text_width(text) + 12, h = 18;
    if(filled) {
        pd->graphics->fillRoundRect(x, y, w, h, 5, kColorBlack);
        pd->graphics->setDrawMode(kDrawModeFillWhite);
        pd->graphics->drawText(text, strlen(text), kASCIIEncoding, x + 6, y + 1);
        pd->graphics->setDrawMode(kDrawModeCopy);
    } else {
        pd->graphics->drawRoundRect(x, y, w, h, 5, 1, kColorBlack);
        pd->graphics->drawText(text, strlen(text), kASCIIEncoding, x + 6, y + 1);
    }
    return w;
}

/* Truncates to fit `max_w` pixels, ending in ".." when cut. */
static void fit_text(char* out, size_t cap, const char* in, int max_w) {
    strncpy(out, in, cap - 1);
    out[cap - 1] = 0;
    if(text_width(out) <= max_w) return;
    size_t n = strlen(out);
    if(n + 3 > cap) n = cap - 3;
    while(n > 1) {
        n--;
        out[n] = 0;
        strcpy(out + n, "..");
        if(text_width(out) <= max_w) return;
        out[n] = 0;
    }
}

static void draw_panel(void) {
    sstv_demod_status_t st;
    sstv_demod_status(&demod, &st);
    int x0 = LCD_COLUMNS - PANEL_W;
    pd->graphics->fillRect(x0, 0, PANEL_W, LCD_ROWS, kColorWhite);
    pd->graphics->drawLine(x0, 0, x0, LCD_ROWS, 1, kColorBlack);
    int x = x0 + 8, y = 8;
    char t[64];
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    bool quiet = segs_done > 0 && !finished && now - last_data_ms > LOST_TIMEOUT_MS;

    if(mic_state < 0) {
        draw_text("mic denied", x, y);
        return;
    }
    if(mic_state == 0) {
        draw_text("mic permission?", x, y);
        return;
    }
    /* name */
    fit_text(t, sizeof t, current_name, PANEL_W - 16);
    draw_text(t, x, y);
    y += 20;
    /* state pill */
    const char* state = finished ? "DONE" : segs_done == SSTV_NUM_SEGS ? "DONE" : quiet ? "LOST" : segs_done > 0 ? "RX" : "IDLE";
    int px = x;
    px += draw_pill(state, px, y, true) + 6;
    px += draw_pill(rx.coded ? "ZIP" : "RAW", px, y, false) + 6;
    if(rec_len >= 0) px += draw_pill("REC", px, y, true) + 6;
    else if(image_saved) px += draw_pill("SAVED", px, y, false) + 6;
    y += 26;
    /* progress bar of lines */
    int bar_w = PANEL_W - 16;
    pd->graphics->drawRect(x, y, bar_w, 10, kColorBlack);
    int fill = (bar_w - 2) * segs_done / SSTV_NUM_SEGS;
    if(fill > 0) pd->graphics->fillRect(x + 1, y + 1, fill, 8, kColorBlack);
    y += 14;
    snprintf(t, sizeof t, "%d / %d lines", segs_done / 2, SSTV_HEIGHT);
    draw_text(t, x, y);
    y += 22;
    /* stats, label left / value right */
    struct { const char* k; int v; } rows[] = {
        {"frames ok", rx.frames_ok}, {"frames bad", rx.frames_bad}, {"rs fixed", rx.rs_corrected},
        {"flywheel", demod.flywheel_frames}, {"dropped", (int)mic_dropped},
    };
    for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        draw_text(rows[i].k, x, y);
        snprintf(t, sizeof t, "%d", rows[i].v);
        draw_text(t, x0 + PANEL_W - 8 - text_width(t), y);
        y += 17;
    }
    /* signal level */
    y += 4;
    static const char tone_names[] = "0123S";
    snprintf(t, sizeof t, "tone %c", st.tone >= 0 && st.tone <= SSTV_NUM_TONES ? tone_names[st.tone] : '-');
    draw_text(t, x, y);
    int lb_x = x + 60, lb_w = PANEL_W - 76;
    pd->graphics->drawRect(lb_x, y + 4, lb_w, 8, kColorBlack);
    int lf = (int)(st.level * (float)(lb_w - 2));
    if(lf > 0) pd->graphics->fillRect(lb_x + 1, y + 5, lf, 6, kColorBlack);
    /* hints */
    pd->graphics->drawLine(x0, LCD_ROWS - 22, LCD_COLUMNS, LCD_ROWS - 22, 1, kColorBlack);
    draw_text("A clear   B gallery", x, LCD_ROWS - 18);
}

/* Banner over the image for a few seconds after the end-of-transmission frame. */
static void draw_finished_banner(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(!finished || now - finished_ms > 6000) return;
    char t[80];
    snprintf(t, sizeof t, "Received %s  (%d/%d lines)", current_name, finished_lines, SSTV_HEIGHT);
    int w = text_width(t) + 24, h = 34;
    int x = (LCD_COLUMNS - w) / 2, y = LCD_ROWS - h - 14;
    pd->graphics->fillRoundRect(x, y, w, h, 6, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->drawText(t, strlen(t), kASCIIEncoding, x + 12, y + 8);
    pd->graphics->setDrawMode(kDrawModeCopy);
}

static void draw_grid(void) {
    pd->graphics->clear(kColorWhite);
    /* title bar */
    pd->graphics->fillRect(0, 0, LCD_COLUMNS, 24, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    char t[48];
    snprintf(t, sizeof t, "Gallery  %d image%s", gallery_count, gallery_count == 1 ? "" : "s");
    pd->graphics->drawText(t, strlen(t), kASCIIEncoding, 8, 4);
    const char* hint = "A open   B back";
    pd->graphics->drawText(hint, strlen(hint), kASCIIEncoding, LCD_COLUMNS - 8 - text_width(hint), 4);
    pd->graphics->setDrawMode(kDrawModeCopy);
    if(gallery_count == 0) {
        draw_text("no saved images yet", 130, 110);
        return;
    }
    int cell_w = LCD_COLUMNS / GRID_COLS, cell_h = (LCD_ROWS - 24) / GRID_ROWS;
    gallery_ensure_thumbs(gallery_page, GRID_PER_PAGE);
    for(int k = 0; k < GRID_PER_PAGE; k++) {
        int idx = gallery_page + k;
        if(idx >= gallery_count) break;
        int cx = (k % GRID_COLS) * cell_w + (cell_w - THUMB_W) / 2;
        int cy = 24 + (k / GRID_COLS) * cell_h + 10;
        if(thumbs[idx]) pd->graphics->drawBitmap(thumbs[idx], cx, cy, kBitmapUnflipped);
        pd->graphics->drawRect(cx - 1, cy - 1, THUMB_W + 2, THUMB_H + 2, kColorBlack);
        if(idx == gallery_index) {
            pd->graphics->drawRect(cx - 4, cy - 4, THUMB_W + 8, THUMB_H + 8, kColorBlack);
            pd->graphics->drawRect(cx - 3, cy - 3, THUMB_W + 6, THUMB_H + 6, kColorBlack);
        }
        fit_text(t, sizeof t, gallery_names[idx], cell_w - 8);
        draw_text(t, (k % GRID_COLS) * cell_w + (cell_w - text_width(t)) / 2, cy + THUMB_H + 6);
    }
    if(gallery_count > GRID_PER_PAGE) {
        snprintf(t, sizeof t, "page %d/%d", gallery_page / GRID_PER_PAGE + 1, (gallery_count + GRID_PER_PAGE - 1) / GRID_PER_PAGE);
        draw_text(t, LCD_COLUMNS - 8 - text_width(t), LCD_ROWS - 18);
    }
}

static void draw_full(void) {
    pd->graphics->clear(kColorWhite);
    if(gallery_bitmap) pd->graphics->drawBitmap(gallery_bitmap, 0, 0, kBitmapUnflipped);
    char t[64];
    fit_text(t, sizeof t, gallery_names[gallery_index], 200);
    int w = text_width(t) + 16;
    pd->graphics->fillRoundRect(8, LCD_ROWS - 28, w, 20, 5, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    pd->graphics->drawText(t, strlen(t), kASCIIEncoding, 16, LCD_ROWS - 26);
    pd->graphics->setDrawMode(kDrawModeCopy);
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

    if(ui_mode == ModeFull) {
        if(pushed & kButtonB) ui_mode = ModeGrid;
        if((pushed & kButtonRight) && gallery_count > 0) { gallery_index = (gallery_index + 1) % gallery_count; gallery_load_full(gallery_index); }
        if((pushed & kButtonLeft) && gallery_count > 0) { gallery_index = (gallery_index + gallery_count - 1) % gallery_count; gallery_load_full(gallery_index); }
        draw_full();
        return 1;
    }
    if(ui_mode == ModeGrid) {
        if(pushed & kButtonB) ui_mode = ModeReceive;
        if(gallery_count > 0) {
            if(pushed & kButtonRight) gallery_index = (gallery_index + 1) % gallery_count;
            if(pushed & kButtonLeft) gallery_index = (gallery_index + gallery_count - 1) % gallery_count;
            if(pushed & kButtonDown) gallery_index = (gallery_index + GRID_COLS) % gallery_count;
            if(pushed & kButtonUp) gallery_index = (gallery_index + gallery_count - GRID_COLS) % gallery_count;
            gallery_page = gallery_index / GRID_PER_PAGE * GRID_PER_PAGE;
            if((pushed & kButtonA) && gallery_load_full(gallery_index)) ui_mode = ModeFull;
        }
        draw_grid();
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
    draw_finished_banner();
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
