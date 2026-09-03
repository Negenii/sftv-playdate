#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_frame.h"
#include "mic_ring.h"

#define LOST_TIMEOUT_MS 5000
#define PANEL_W 140
#define MAX_GALLERY 64
#define NAME_CAP 40

static PlaydateAPI* pd;
static LCDBitmap* image; /* the received picture, composed with the preview placeholder */
static uint8_t* image_data;
static int image_rowbytes;

static LCDFont* font_ui; /* Roobert 10 Bold: panel, labels */
static LCDFont* font_num; /* Roobert 9 Mono Condensed: counters */
static LCDFont* font_big; /* Roobert 20 Medium: titles */
static LCDBitmap* dolphin; /* 124x128, faces right */
static LCDBitmapTable* crank_frames; /* SDK crank indicator, 12 frames 52x38 */
static LCDBitmap* thumb_mask; /* rounded corners for thumbnails */

static sstv_demod demod;
static sstv_rx rx;
static mic_ring ring;

/* Receive state. Half-line segments (raw mode) or whole lines (coded mode) arrive verified. */
static uint8_t seg_seen[SSTV_HEIGHT]; /* bit 0: left half, bit 1: right half */
static int segs_done; /* of SSTV_NUM_SEGS */
static int lines_reached; /* highest line with data + 1: how much of the sheet is on screen */
static float sheet_y = LCD_ROWS; /* top of the sheet; it slides in from the bottom edge */
static unsigned last_data_ms;
static char current_name[NAME_CAP] = "untitled";
static bool started; /* START meta received, transmission in progress */
static unsigned started_ms; /* when the START block ended: the pass timeline starts here */
static int total_frames; /* physical frames in the pass, announced in START */
static bool image_dirty; /* something received since the last save */
static bool finished; /* END meta received */
static unsigned finished_ms;
static int finished_lines;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool show_panel; /* side panel with statistics, toggled with Right */
static unsigned led_frame_ms; /* virtual LED: last physical frame time */
static unsigned led_block_ms; /* last decoded block time */
static volatile unsigned mic_dropped;

/* Previews (PBM packing) arriving right after START, progressive-JPEG style: first the 40x20
 * tiny layer (one block), then the 80x48 layer (five blocks). */
static uint8_t tiny_bits[SSTV_TINY_BYTES];
static bool tiny_have[SSTV_TINY_CHUNKS];
static uint8_t preview_bits[SSTV_PREVIEW_BYTES];
static bool preview_have[SSTV_PREVIEW_CHUNKS];
static int preview_chunks; /* of SSTV_INTRO_FRAMES, both layers */
static uint8_t schedule[SSTV_MAX_CODED_BLOCKS]; /* coded pass: lines per data block */
static bool schedule_have[SSTV_SCHEDULE_CHUNKS];
static LCDBitmap* preview_bitmap; /* 80x48 for the console icon in the intro scene */

/* Screens. */
typedef enum { ModeMenu, ModeReceive, ModeGrid, ModeFull } UiMode;
static UiMode ui_mode = ModeMenu;
static int menu_index; /* 0 receive, 1 gallery */
static char gallery_names[MAX_GALLERY][NAME_CAP];
static int gallery_count;
static int gallery_index; /* cursor */
static int gallery_page; /* first tile shown */
static LCDBitmap* gallery_bitmap;
static LCDBitmap* thumbs[MAX_GALLERY]; /* 100x60 thumbnails, loaded lazily */
static LCDBitmap* menu_thumb; /* latest saved image, for the gallery tile */
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define THUMB_W 100
#define THUMB_H 60
#define THUMB_R 8

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream (Data/rec.wav). */
#define REC_SECONDS 30
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */

/* ---- audio thread ---- */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    int stored = mic_ring_write(&ring, data, len);
    if(stored < len) mic_dropped += (unsigned)(len - stored);
    return 1;
}

/* ---- names ---- */

/* Display form: file name without the .pbm extension. */
static void display_name(char* out, size_t cap, const char* in) {
    strncpy(out, in, cap - 1);
    out[cap - 1] = 0;
    size_t n = strlen(out);
    if(n > 4 && strcmp(out + n - 4, ".pbm") == 0) out[n - 4] = 0;
}

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

/* ---- saving ---- */

/* Saves the received picture as a P4 PBM under Data/. Same name overwrites. Segments that
 * never arrived are written white, not with the preview placeholder. */
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
        for(int i = 0; i < SSTV_LINE_BYTES; i++) {
            bool have = seg_seen[y] & (i < SSTV_SEG_BYTES ? 1 : 2);
            row[i] = have ? (uint8_t)~image_data[y * image_rowbytes + i] : 0;
        }
        pd->file->write(f, row, SSTV_LINE_BYTES);
    }
    pd->file->close(f);
    image_dirty = false;
}

/* ---- gallery data ---- */

static void gallery_list_cb(const char* path, void* userdata) {
    (void)userdata;
    size_t n = strlen(path);
    if(n < 5 || strcmp(path + n - 4, ".pbm") != 0 || gallery_count >= MAX_GALLERY) return;
    strncpy(gallery_names[gallery_count], path, NAME_CAP - 1);
    gallery_names[gallery_count][NAME_CAP - 1] = 0;
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

/* 4:1 nearest-neighbour thumbnail (400x240 -> 100x60) with rounded corners. */
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
    if(thumb_mask) pd->graphics->setBitmapMask(t, thumb_mask);
    return t;
}

static void free_thumbs(void) {
    for(int i = 0; i < MAX_GALLERY; i++) {
        if(thumbs[i]) pd->graphics->freeBitmap(thumbs[i]);
        thumbs[i] = NULL;
    }
}

static void gallery_scan(void) {
    save_image();
    free_thumbs();
    gallery_count = 0;
    pd->file->listfiles(".", gallery_list_cb, NULL, 0);
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
    gallery_scan();
    ui_mode = ModeGrid;
    gallery_index = 0;
    gallery_page = 0;
    gallery_ensure_thumbs(0, GRID_PER_PAGE);
}

static void menu_open(void) {
    gallery_scan();
    if(menu_thumb) {
        pd->graphics->freeBitmap(menu_thumb);
        menu_thumb = NULL;
    }
    if(gallery_count > 0) {
        uint8_t* px = pd->system->realloc(NULL, SSTV_IMAGE_BYTES);
        if(read_pbm(gallery_names[gallery_count - 1], px)) menu_thumb = make_thumb(px);
        pd->system->realloc(px, 0);
    }
    ui_mode = ModeMenu;
}

/* ---- receive callbacks (main thread) ---- */

/* Best preview bit for picture pixel (x, line): the 80x48 layer if its row has arrived, else
 * the 40x20 layer, else white (0). */
static bool preview_row_ok(int py) {
    return preview_have[py * SSTV_PREVIEW_LINE_BYTES / SSTV_PREVIEW_CHUNK] &&
           preview_have[(py * SSTV_PREVIEW_LINE_BYTES + SSTV_PREVIEW_LINE_BYTES - 1) / SSTV_PREVIEW_CHUNK];
}

static bool tiny_row_ok(int ty) {
    return tiny_have[ty * SSTV_TINY_LINE_BYTES / SSTV_PREVIEW_CHUNK];
}

static int placeholder_bit(int x, int line) {
    int py = line / (SSTV_HEIGHT / SSTV_PREVIEW_H);
    if(preview_row_ok(py)) {
        int px = x / (SSTV_WIDTH / SSTV_PREVIEW_W);
        return (preview_bits[py * SSTV_PREVIEW_LINE_BYTES + (px >> 3)] >> (7 - (px & 7))) & 1;
    }
    int ty = line / (SSTV_HEIGHT / SSTV_TINY_H);
    if(tiny_row_ok(ty)) {
        int tx = x / (SSTV_WIDTH / SSTV_TINY_W);
        return (tiny_bits[ty * SSTV_TINY_LINE_BYTES + (tx >> 3)] >> (7 - (tx & 7))) & 1;
    }
    return 0;
}

/* Paints the placeholder into picture line `line` wherever no real pixels have arrived:
 * black preview pixels become a 50 % checker so real lines read as sharper. */
static void paint_placeholder_line(int line) {
    uint8_t* dst = image_data + line * image_rowbytes;
    for(int half = 0; half < 2; half++) {
        if(seg_seen[line] & (1 << half)) continue;
        for(int i = half * SSTV_SEG_BYTES; i < (half + 1) * SSTV_SEG_BYTES; i++) {
            uint8_t v = 0xFF;
            for(int b = 0; b < 8; b++) {
                int x = i * 8 + b;
                if(placeholder_bit(x, line) && ((x + line) & 1) == 0) v &= (uint8_t)~(0x80 >> b);
            }
            dst[i] = v;
        }
    }
}

/* The console icon's screen: the 80x48 layer where it has arrived, the tiny layer elsewhere. */
static void preview_refresh_bitmap(void) {
    if(!preview_bitmap) preview_bitmap = pd->graphics->newBitmap(SSTV_PREVIEW_W, SSTV_PREVIEW_H, kColorWhite);
    int w, h, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(preview_bitmap, &w, &h, &rb, NULL, &data);
    for(int y = 0; y < SSTV_PREVIEW_H; y++) {
        for(int x = 0; x < SSTV_PREVIEW_W; x++) {
            int bit = placeholder_bit(x * (SSTV_WIDTH / SSTV_PREVIEW_W), y * (SSTV_HEIGHT / SSTV_PREVIEW_H));
            if(bit) data[y * rb + (x >> 3)] &= (uint8_t)~(0x80 >> (x & 7));
            else data[y * rb + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        }
    }
}

static void on_preview(void* ctx, int layer, int chunk, int nbytes, const uint8_t* data) {
    (void)ctx;
    if(layer == 2) {
        if(chunk < 0 || chunk >= SSTV_SCHEDULE_CHUNKS) return;
        memcpy(schedule + chunk * SSTV_PREVIEW_CHUNK, data, (size_t)nbytes);
        schedule_have[chunk] = true;
        last_data_ms = pd->system->getCurrentTimeMilliseconds();
        return;
    }
    if(layer == 0) {
        if(chunk < 0 || chunk >= SSTV_TINY_CHUNKS) return;
        memcpy(tiny_bits + chunk * SSTV_PREVIEW_CHUNK, data, (size_t)nbytes);
        if(!tiny_have[chunk]) preview_chunks++;
        tiny_have[chunk] = true;
    } else {
        if(chunk < 0 || chunk >= SSTV_PREVIEW_CHUNKS) return;
        memcpy(preview_bits + chunk * SSTV_PREVIEW_CHUNK, data, (size_t)nbytes);
        if(!preview_have[chunk]) preview_chunks++;
        preview_have[chunk] = true;
    }
    preview_refresh_bitmap();
    for(int line = 0; line < SSTV_HEIGHT; line++) paint_placeholder_line(line);
    last_data_ms = pd->system->getCurrentTimeMilliseconds();
}

static void on_pixels(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels) {
    (void)ctx;
    last_data_ms = pd->system->getCurrentTimeMilliseconds();
    if(line >= SSTV_HEIGHT || x_byte + nbytes > SSTV_LINE_BYTES) return;
    uint8_t* dst = image_data + line * image_rowbytes + x_byte;
    /* PBM: 1 = black. Playdate bitmap: 1 = white. */
    for(int i = 0; i < nbytes; i++) dst[i] = (uint8_t)~pixels[i];
    if(line + 1 > lines_reached) lines_reached = line + 1;
    uint8_t mask = nbytes == SSTV_LINE_BYTES ? 3 : (x_byte == 0 ? 1 : 2);
    uint8_t before = seg_seen[line];
    seg_seen[line] |= mask;
    segs_done += ((seg_seen[line] & 1) - (before & 1)) + (((seg_seen[line] >> 1) & 1) - ((before >> 1) & 1));
    image_dirty = true;
}

static void clear_image(void) {
    memset(image_data, 0xFF, (size_t)image_rowbytes * SSTV_HEIGHT);
    memset(seg_seen, 0, sizeof seg_seen);
    memset(preview_bits, 0, sizeof preview_bits);
    memset(preview_have, 0, sizeof preview_have);
    memset(tiny_bits, 0, sizeof tiny_bits);
    memset(tiny_have, 0, sizeof tiny_have);
    memset(schedule, 0, sizeof schedule);
    memset(schedule_have, 0, sizeof schedule_have);
    preview_chunks = 0;
    if(preview_bitmap) preview_refresh_bitmap();
    segs_done = 0;
    lines_reached = 0;
    sheet_y = LCD_ROWS;
    image_dirty = false;
    finished = false;
    started = false;
    sstv_demod_reset_counters(&demod);
    sstv_rx_reset_counters(&rx);
}

static void finish(unsigned now) {
    finished = true;
    started = false;
    finished_ms = now;
    finished_lines = segs_done / 2;
    lines_reached = SSTV_HEIGHT; /* pull the whole sheet in, gaps and all */
    save_image();
}

/* Lines the transmitter has sent by now according to the announced timeline: the START block
 * ended at started_ms, then the preview frames (and, coded, the schedule block), then the data
 * frames, then the END block. Raw passes carry two lines per block; coded passes follow the
 * schedule (lines per block) where it arrived and spread lines evenly where it did not.
 * Drives the sheet so lost frames do not stall it. */
static int expected_lines(unsigned now) {
    if(!started || total_frames <= 0) return 0;
    int intro = rx.coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES;
    int data_frames = total_frames - 2 * SSTV_ILEAVE - intro;
    if(data_frames <= 0) return 0;
    float frames = (float)(now - started_ms) * 1000.0f / (float)SSTV_PHYS_US - (float)intro;
    if(frames <= 0) return 0;
    if(frames >= (float)data_frames) return SSTV_HEIGHT;
    int blocks = data_frames / SSTV_ILEAVE;
    float fblock = frames / (float)SSTV_ILEAVE; /* data blocks elapsed, fractional */
    int b = (int)fblock;
    float frac = fblock - (float)b;
    float lines = 0;
    for(int i = 0; i <= b && i < blocks; i++) {
        float n;
        if(!rx.coded) n = (float)SSTV_HEIGHT / (float)blocks;
        else if(i < SSTV_MAX_CODED_BLOCKS && schedule_have[i / SSTV_PREVIEW_CHUNK]) n = (float)schedule[i];
        else n = (float)SSTV_HEIGHT / (float)blocks;
        lines += (i < b) ? n : n * frac;
    }
    if(lines > (float)SSTV_HEIGHT) lines = (float)SSTV_HEIGHT;
    return (int)lines;
}

static void on_meta(void* ctx, const char* name, bool is_end, int frames, bool coded) {
    (void)ctx; (void)coded;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(is_end) {
        finish(now); /* the transmitter is done: save and announce */
        return;
    }
    /* A new transmission starts: keep what the previous one produced, then start fresh. */
    save_image();
    clear_image();
    started = true;
    started_ms = now;
    total_frames = frames;
    strncpy(current_name, name, NAME_CAP - 1);
    current_name[NAME_CAP - 1] = 0;
    last_data_ms = now;
    /* Whatever screen is up, an incoming picture takes over. */
    ui_mode = ModeReceive;
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

/* ---- drawing helpers ---- */

static LCDFont* cur_font;

static void use_font(LCDFont* f) {
    cur_font = f;
    if(f) pd->graphics->setFont(f);
}

static int text_width(const char* text) {
    return pd->graphics->getTextWidth(cur_font, text, strlen(text), kASCIIEncoding, 0);
}

static int font_height(void) {
    return cur_font ? pd->graphics->getFontHeight(cur_font) : 16;
}

static void draw_text(const char* text, int x, int y) {
    pd->graphics->drawText(text, strlen(text), kASCIIEncoding, x, y);
}

static void draw_text_white(const char* text, int x, int y) {
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    draw_text(text, x, y);
    pd->graphics->setDrawMode(kDrawModeCopy);
}

static void draw_text_centered(const char* text, int cx, int y) {
    draw_text(text, cx - text_width(text) / 2, y);
}

/* Rounded label in the current font; filled = black pill with white text. Returns width. */
static int draw_pill(const char* text, int x, int y, bool filled) {
    int h = font_height() + 4, w = text_width(text) + 12;
    if(filled) {
        pd->graphics->fillRoundRect(x, y, w, h, h / 2, kColorBlack);
        draw_text_white(text, x + 6, y + 2);
    } else {
        pd->graphics->drawRoundRect(x, y, w, h, h / 2, 1, kColorBlack);
        draw_text(text, x + 6, y + 2);
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

/* Black title bar with a left title and a right hint. */
static void draw_title_bar(const char* title, const char* hint) {
    pd->graphics->fillRect(0, 0, LCD_COLUMNS, 24, kColorBlack);
    use_font(font_ui);
    draw_text_white(title, 8, 5);
    if(hint) draw_text_white(hint, LCD_COLUMNS - 32 - text_width(hint), 5);
}

/* A Playdate drawn at (x, y) in the proportions of the real one (76x74 mm body scaled so the
 * 80x48 preview fills the screen): body 104x100, screen top left, d-pad bottom left, A/B
 * bottom right, lock button on top, crank on the right side (SDK indicator frames; `crank`
 * selects the frame, so it turns while a picture is coming in). */
#define CONSOLE_W 104
#define CONSOLE_H 100
#define CONSOLE_SCREEN_X 12
#define CONSOLE_SCREEN_Y 11

static void draw_console(int x, int y, LCDBitmap* screen, int crank) {
    /* crank behind the body so its hinge tucks under the edge */
    if(crank_frames) {
        LCDBitmap* f = pd->graphics->getTableBitmap(crank_frames, crank % 12);
        if(f) pd->graphics->drawBitmap(f, x + CONSOLE_W - 8, y + 24, kBitmapUnflipped);
    }
    /* lock button on the top edge */
    pd->graphics->fillRoundRect(x + 74, y - 3, 14, 6, 2, kColorBlack);
    /* body: 2 px outline */
    pd->graphics->fillRoundRect(x - 2, y - 2, CONSOLE_W + 4, CONSOLE_H + 4, 12, kColorBlack);
    pd->graphics->fillRoundRect(x, y, CONSOLE_W, CONSOLE_H, 10, kColorWhite);
    /* screen with a thin bezel */
    int sx = x + CONSOLE_SCREEN_X, sy = y + CONSOLE_SCREEN_Y;
    pd->graphics->fillRect(sx - 2, sy - 2, SSTV_PREVIEW_W + 4, SSTV_PREVIEW_H + 4, kColorBlack);
    pd->graphics->fillRect(sx, sy, SSTV_PREVIEW_W, SSTV_PREVIEW_H, kColorWhite);
    if(screen) pd->graphics->drawBitmap(screen, sx, sy, kBitmapUnflipped);
    /* d-pad: two rounded bars */
    int dx = x + 28, dy = y + 80;
    pd->graphics->fillRoundRect(dx - 12, dy - 4, 24, 8, 3, kColorBlack);
    pd->graphics->fillRoundRect(dx - 4, dy - 12, 8, 24, 3, kColorBlack);
    pd->graphics->fillRect(dx - 2, dy - 2, 4, 4, kColorWhite);
    /* B lower left, A upper right */
    pd->graphics->fillEllipse(x + 60, y + 78, 14, 14, 0.0f, 0.0f, kColorBlack);
    pd->graphics->fillEllipse(x + 78, y + 66, 14, 14, 0.0f, 0.0f, kColorBlack);
    /* speaker slot under the screen */
    for(int i = 0; i < 5; i++) pd->graphics->fillRect(x + 84 + i * 3, y + 66, 1, 1, kColorBlack);
}

/* Water arc from the dolphin's blowhole to the console screen; `t` drives the droplets. */
static void draw_spout(int x0, int y0, int x1, int y1, float t) {
    const int drops = 9;
    for(int k = 0; k < drops; k++) {
        float u = fmodf(t + (float)k / (float)drops, 1.0f);
        float x = (float)x0 + ((float)(x1 - x0)) * u;
        float y = (float)y0 + ((float)(y1 - y0)) * u - 70.0f * sinf(3.14159f * u);
        int r = 2 + (int)(2.0f * sinf(3.14159f * u));
        pd->graphics->fillEllipse((int)x - r, (int)y - r, 2 * r, 2 * r, 0.0f, 0.0f, kColorBlack);
    }
}

/* ---- screens ---- */

static void draw_menu(void) {
    pd->graphics->clear(kColorWhite);
    use_font(font_big);
    draw_text("SSTV", 16, 8);
    use_font(font_ui);
    const char* mic = mic_state > 0 ? "mic on" : mic_state < 0 ? "mic denied" : "mic?";
    draw_pill(mic, LCD_COLUMNS - 32 - text_width(mic) - 12, 10, false);

    const int tile_w = 170, tile_h = 170, ty = 52;
    const int tx[2] = {20, LCD_COLUMNS - 20 - tile_w};
    const char* labels[2] = {"Receive", "Gallery"};
    for(int i = 0; i < 2; i++) {
        bool sel = i == menu_index;
        pd->graphics->fillRoundRect(tx[i], ty, tile_w, tile_h, 14, kColorBlack);
        pd->graphics->fillRoundRect(tx[i] + (sel ? 3 : 1), ty + (sel ? 3 : 1), tile_w - (sel ? 6 : 2), tile_h - (sel ? 6 : 2), sel ? 11 : 13, kColorWhite);
        if(i == 0) {
            if(dolphin) {
                pd->graphics->setDrawMode(kDrawModeInverted);
                pd->graphics->drawBitmap(dolphin, tx[i] + (tile_w - 124) / 2, ty + 6, kBitmapUnflipped);
                pd->graphics->setDrawMode(kDrawModeCopy);
            }
        } else if(menu_thumb) {
            pd->graphics->drawBitmap(menu_thumb, tx[i] + (tile_w - THUMB_W) / 2, ty + 38, kBitmapUnflipped);
            pd->graphics->drawRoundRect(tx[i] + (tile_w - THUMB_W) / 2 - 1, ty + 37, THUMB_W + 2, THUMB_H + 2, THUMB_R, 1, kColorBlack);
        } else {
            /* empty gallery: a stack of frames */
            for(int k = 2; k >= 0; k--) {
                int ox = tx[i] + (tile_w - 80) / 2 + k * 6, oy = ty + 40 + k * 6;
                pd->graphics->fillRoundRect(ox, oy, 80, 48, 5, kColorWhite);
                pd->graphics->drawRoundRect(ox, oy, 80, 48, 5, 1, kColorBlack);
            }
        }
        use_font(font_big);
        if(sel) {
            int w = text_width(labels[i]) + 20, h = 30;
            int px = tx[i] + (tile_w - w) / 2, py = ty + tile_h - 40;
            pd->graphics->fillRoundRect(px, py, w, h, 15, kColorBlack);
            draw_text_white(labels[i], px + 10, py + 3);
        } else {
            draw_text_centered(labels[i], tx[i] + tile_w / 2, ty + tile_h - 37);
        }
    }
    use_font(font_ui);
    if(gallery_count > 0) {
        char t[32];
        snprintf(t, sizeof t, "%d saved", gallery_count);
        draw_text_centered(t, tx[1] + tile_w / 2, ty + tile_h + 4);
    }
}

/* Intro scene: dolphin sends the preview to a Playdate. Shown while listening and until the
 * first picture lines arrive. */
static void draw_scene(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    pd->graphics->clear(kColorWhite);
    const int dx = 24, dy = 60; /* dolphin */
    const int cx = 232, cy = 70; /* console */
    if(dolphin) {
        pd->graphics->setDrawMode(kDrawModeInverted);
        pd->graphics->drawBitmap(dolphin, dx, dy, kBitmapUnflipped);
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
    draw_console(cx, cy, started ? preview_bitmap : NULL, started ? (int)(now / 80) : 5);
    use_font(font_ui);
    char t[64], nm[NAME_CAP];
    if(started) {
        draw_spout(dx + 78, dy + 14, cx + CONSOLE_SCREEN_X + SSTV_PREVIEW_W / 2, cy + CONSOLE_SCREEN_Y + SSTV_PREVIEW_H / 2, (float)(now % 1200) / 1200.0f);
        display_name(nm, sizeof nm, current_name);
        use_font(font_big);
        draw_text_centered(nm, LCD_COLUMNS / 2, 190);
        use_font(font_ui);
        int secs = total_frames > 0 ? (int)((unsigned)total_frames * (SSTV_PHYS_US / 1000) / 1000) : 0;
        snprintf(t, sizeof t, "%s, %d:%02d, preview %d/%d", rx.coded ? "compressed" : "raw", secs / 60, secs % 60, preview_chunks, SSTV_INTRO_FRAMES);
        draw_text_centered(t, LCD_COLUMNS / 2, 218);
    } else {
        use_font(font_big);
        draw_text_centered("Listening", LCD_COLUMNS / 2, 190);
        use_font(font_ui);
        const char* hint = mic_state > 0 ? "start a transmission on the Flipper" : mic_state < 0 ? "microphone access denied" : "waiting for microphone permission";
        draw_text_centered(hint, LCD_COLUMNS / 2, 218);
    }
}

static void draw_panel(void) {
    sstv_demod_status_t st;
    sstv_demod_status(&demod, &st);
    int x0 = LCD_COLUMNS - PANEL_W;
    pd->graphics->fillRect(x0, 0, PANEL_W, LCD_ROWS, kColorWhite);
    pd->graphics->drawLine(x0, 0, x0, LCD_ROWS, 1, kColorBlack);
    int x = x0 + 8, y = 8, right = x0 + PANEL_W - 8;
    char t[64], nm[NAME_CAP];
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    bool quiet = segs_done > 0 && !finished && now - last_data_ms > LOST_TIMEOUT_MS;
    use_font(font_ui);
    int lh = font_height() + 3;

    display_name(nm, sizeof nm, current_name);
    fit_text(t, sizeof t, nm, PANEL_W - 16 - 20);
    draw_text(t, x, y);
    y += lh + 4;
    /* state and mode pills */
    const char* state = finished ? "DONE" : segs_done == SSTV_NUM_SEGS ? "DONE" : quiet ? "LOST" : segs_done > 0 || started ? "RX" : "IDLE";
    int px = x;
    px += draw_pill(state, px, y, true) + 5;
    px += draw_pill(rx.coded ? "ZIP" : "RAW", px, y, false) + 5;
    if(rec_len >= 0) draw_pill("REC", px, y, true);
    y += font_height() + 12;
    /* line progress */
    int bar_w = PANEL_W - 16;
    pd->graphics->drawRoundRect(x, y, bar_w, 8, 4, 1, kColorBlack);
    int fill = (bar_w - 2) * segs_done / SSTV_NUM_SEGS;
    if(fill > 0) pd->graphics->fillRoundRect(x + 1, y + 1, fill, 6, 3, kColorBlack);
    y += 12;
    snprintf(t, sizeof t, "%d / %d lines", segs_done / 2, SSTV_HEIGHT);
    draw_text(t, x, y);
    y += lh + 6;
    /* counters: label left, number right in the mono font */
    struct { const char* k; int v; } rows[] = {
        {"preview", preview_chunks}, {"frames ok", rx.frames_ok}, {"frames bad", rx.frames_bad},
        {"rs fixed", rx.rs_corrected}, {"flywheel", demod.flywheel_frames}, {"dropped", (int)mic_dropped},
    };
    for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        use_font(font_ui);
        draw_text(rows[i].k, x, y);
        use_font(font_num);
        snprintf(t, sizeof t, "%d", rows[i].v);
        draw_text(t, right - text_width(t), y + 1);
        y += lh;
    }
    /* tone and level */
    y += 4;
    use_font(font_ui);
    static const char tone_names[] = "0123S";
    snprintf(t, sizeof t, "tone %c", st.tone >= 0 && st.tone <= SSTV_NUM_TONES ? tone_names[st.tone] : '-');
    draw_text(t, x, y);
    int lb_x = x + 52, lb_w = right - lb_x;
    pd->graphics->drawRoundRect(lb_x, y + 3, lb_w, 8, 4, 1, kColorBlack);
    int lf = (int)(st.level * (float)(lb_w - 2));
    if(lf > 0) pd->graphics->fillRoundRect(lb_x + 1, y + 4, lf, 6, 3, kColorBlack);
    /* hints */
    pd->graphics->drawLine(x0, LCD_ROWS - 20, LCD_COLUMNS, LCD_ROWS - 20, 1, kColorBlack);
    draw_text("A clear   B menu", x, LCD_ROWS - 16);
}

/* Banner over the picture for a few seconds after the END frame. */
static void draw_finished_banner(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(!finished || now - finished_ms > 6000) return;
    char t[80], nm[NAME_CAP];
    display_name(nm, sizeof nm, current_name);
    use_font(font_ui);
    snprintf(t, sizeof t, "Received %s   %d/%d lines", nm, finished_lines, SSTV_HEIGHT);
    int w = text_width(t) + 24, h = font_height() + 16;
    int x = (LCD_COLUMNS - w) / 2, y = LCD_ROWS - h - 14;
    pd->graphics->fillRoundRect(x, y, w, h, h / 2, kColorBlack);
    draw_text_white(t, x + 12, y + 8);
}

/* The picture is a sheet that crawls in from the bottom edge as lines arrive (the Flipper
 * shows the same sheet leaving through its top edge). It covers the intro scene as it rises. */
static void draw_receive(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    int shown = expected_lines(now);
    if(lines_reached > shown) shown = lines_reached; /* never hide a line that arrived */
    float target = (float)(LCD_ROWS - shown);
    float d = target - sheet_y;
    if(d < -0.5f || d > 0.5f) sheet_y += d * 0.15f;
    else sheet_y = target;
    int y = (int)(sheet_y + 0.5f);
    if(y > 0) draw_scene(); /* listening, or START + preview received; the sheet grows over it */
    else pd->graphics->clear(kColorWhite);
    if(y < LCD_ROWS) {
        pd->graphics->drawBitmap(image, 0, y, kBitmapUnflipped);
        if(y > 0) pd->graphics->fillRect(0, y - 2, LCD_COLUMNS, 2, kColorBlack); /* sheet edge */
    }
    if(show_panel) draw_panel();
    draw_finished_banner();
}

static void draw_grid(void) {
    pd->graphics->clear(kColorWhite);
    char t[48], nm[NAME_CAP];
    snprintf(t, sizeof t, "Gallery   %d image%s", gallery_count, gallery_count == 1 ? "" : "s");
    draw_title_bar(t, "A open   B back");
    use_font(font_ui);
    if(gallery_count == 0) {
        draw_text_centered("no saved images yet", LCD_COLUMNS / 2, 110);
        return;
    }
    int cell_w = LCD_COLUMNS / GRID_COLS, cell_h = (LCD_ROWS - 24) / GRID_ROWS;
    gallery_ensure_thumbs(gallery_page, GRID_PER_PAGE);
    for(int k = 0; k < GRID_PER_PAGE; k++) {
        int idx = gallery_page + k;
        if(idx >= gallery_count) break;
        int cx = (k % GRID_COLS) * cell_w + (cell_w - THUMB_W) / 2;
        int cy = 24 + (k / GRID_COLS) * cell_h + 12;
        if(thumbs[idx]) pd->graphics->drawBitmap(thumbs[idx], cx, cy, kBitmapUnflipped);
        pd->graphics->drawRoundRect(cx - 1, cy - 1, THUMB_W + 2, THUMB_H + 2, THUMB_R, 1, kColorBlack);
        if(idx == gallery_index) pd->graphics->drawRoundRect(cx - 5, cy - 5, THUMB_W + 10, THUMB_H + 10, THUMB_R + 3, 3, kColorBlack);
        display_name(nm, sizeof nm, gallery_names[idx]);
        fit_text(t, sizeof t, nm, cell_w - 12);
        draw_text_centered(t, (k % GRID_COLS) * cell_w + cell_w / 2, cy + THUMB_H + 9);
    }
    if(gallery_count > GRID_PER_PAGE) {
        snprintf(t, sizeof t, "page %d/%d", gallery_page / GRID_PER_PAGE + 1, (gallery_count + GRID_PER_PAGE - 1) / GRID_PER_PAGE);
        draw_text(t, LCD_COLUMNS - 8 - text_width(t), LCD_ROWS - 16);
    }
}

static void draw_full(void) {
    pd->graphics->clear(kColorWhite);
    if(gallery_bitmap) pd->graphics->drawBitmap(gallery_bitmap, 0, 0, kBitmapUnflipped);
    char t[64], nm[NAME_CAP];
    display_name(nm, sizeof nm, gallery_names[gallery_index]);
    use_font(font_ui);
    fit_text(t, sizeof t, nm, 200);
    draw_pill(t, 8, LCD_ROWS - font_height() - 12, true);
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
            }
        }
    }

    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(image_dirty && ((segs_done == SSTV_NUM_SEGS) || (now - last_data_ms > LOST_TIMEOUT_MS))) save_image();
    /* END never came: the pass is over by the announced timeline plus a margin. */
    if(started && total_frames > 0 && now - started_ms > (unsigned)(total_frames - SSTV_ILEAVE) * (SSTV_PHYS_US / 1000) + 4000) finish(now);

    switch(ui_mode) {
    case ModeMenu:
        if(pushed & (kButtonLeft | kButtonRight)) menu_index ^= 1;
        if(pushed & kButtonA) {
            if(menu_index == 0) ui_mode = ModeReceive;
            else gallery_open();
        }
        draw_menu();
        break;
    case ModeReceive:
        if(pushed & kButtonA) clear_image();
        if(pushed & kButtonB) menu_open();
        if(pushed & kButtonRight) show_panel = !show_panel;
        if((pushed & kButtonUp) && rec_len < 0) rec_len = 0;
        draw_receive();
        break;
    case ModeGrid:
        if(pushed & kButtonB) menu_open();
        if(gallery_count > 0) {
            if(pushed & kButtonRight) gallery_index = (gallery_index + 1) % gallery_count;
            if(pushed & kButtonLeft) gallery_index = (gallery_index + gallery_count - 1) % gallery_count;
            if(pushed & kButtonDown) gallery_index = (gallery_index + GRID_COLS) % gallery_count;
            if(pushed & kButtonUp) gallery_index = (gallery_index + gallery_count - GRID_COLS) % gallery_count;
            gallery_page = gallery_index / GRID_PER_PAGE * GRID_PER_PAGE;
            if((pushed & kButtonA) && gallery_load_full(gallery_index)) ui_mode = ModeFull;
        }
        if(ui_mode == ModeGrid) draw_grid();
        else draw_full();
        break;
    case ModeFull:
        if(pushed & kButtonB) ui_mode = ModeGrid;
        if((pushed & kButtonRight) && gallery_count > 0) { gallery_index = (gallery_index + 1) % gallery_count; gallery_load_full(gallery_index); }
        if((pushed & kButtonLeft) && gallery_count > 0) { gallery_index = (gallery_index + gallery_count - 1) % gallery_count; gallery_load_full(gallery_index); }
        if(ui_mode == ModeFull) draw_full();
        else draw_grid();
        break;
    }
    draw_led();
    return 1;
}

static void init(void) {
    const char* err = NULL;
    font_ui = pd->graphics->loadFont("fonts/Roobert-10-Bold", &err);
    font_num = pd->graphics->loadFont("fonts/Roobert-9-Mono-Condensed", &err);
    font_big = pd->graphics->loadFont("fonts/Roobert-20-Medium", &err);
    if(!font_num) font_num = font_ui;
    if(!font_big) font_big = font_ui;
    dolphin = pd->graphics->loadBitmap("images/dolphin", &err);
    crank_frames = pd->graphics->loadBitmapTable("images/crank", &err);

    thumb_mask = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorBlack);
    pd->graphics->pushContext(thumb_mask);
    pd->graphics->fillRoundRect(0, 0, THUMB_W, THUMB_H, THUMB_R, kColorWhite);
    pd->graphics->popContext();

    image = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
    int w, h;
    pd->graphics->getBitmapData(image, &w, &h, &image_rowbytes, NULL, &image_data);
    preview_bitmap = pd->graphics->newBitmap(SSTV_PREVIEW_W, SSTV_PREVIEW_H, kColorWhite);
    rec_buf = pd->system->realloc(NULL, sizeof(int16_t) * REC_SECONDS * REC_RATE);
    rec_len = -1;
    mic_ring_init(&ring);
    sstv_rx_init(&rx, on_pixels, on_meta, on_preview, NULL);
    sstv_demod_init(&demod, 44100.0f, on_phys, NULL);
    clear_image();
    menu_open();

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
