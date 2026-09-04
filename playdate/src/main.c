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
#include "sticky_note.h"

#define LOST_TIMEOUT_MS 5000
#define PANEL_W 200
#define BAR_H 30 /* gallery top bar height (Playdate-style, room for the button glyphs) */
#define SHEET_LEAD 50 /* the drawing frontier leads this many px above the bottom edge */
#define MAX_GALLERY 64
#define NAME_CAP 40

static PlaydateAPI* pd;
static LCDBitmap* image; /* the received picture, composed with the preview placeholder */
static uint8_t* image_data;
static int image_rowbytes;

static LCDFont* font_ui; /* Roobert 10 Bold: panel, labels */
static LCDFont* font_num; /* Roobert 9 Mono Condensed: counters */
static LCDFont* font_big; /* Roobert 20 Medium: titles */
static LCDFont* font_glyph; /* Roobert 11 extended: has the Ⓐ / Ⓑ button glyphs */
static LCDBitmap* dolphin_src; /* 71x49 outline dolphin (firmware L1_Furippa1 frame 0), faces right */
static LCDBitmap* dolphin_tile; /* menu tile and scene: 2x, 142x98 */
static LCDBitmap* dolphin_open_tile; /* legacy, kept for the menu tile fallback */
static LCDBitmap* fr_idle, *fr_signal, *fr_preview, *fr_data, *fr_repair; /* 128x49 receive-scene dolphin poses */
static int last_block_type = -1; static unsigned last_block_ms; /* drives the scene dolphin */
#define DOLPHIN_W 71
#define DOLPHIN_H 49
static LCDBitmap* console; /* 140x118 Playdate silhouette, screen interior at (7,7) 88x53 */
static LCDBitmap* thumb_mask; /* rounded corners for thumbnails */

static sstv_demod demod;
static sstv_rx rx;
static mic_ring ring;

/* Receive state. Half-line segments (raw mode) or whole lines (coded mode) arrive verified. */
static uint8_t seg_seen[SSTV_HEIGHT]; /* bit 0: left half, bit 1: right half */
static int segs_done; /* of SSTV_NUM_SEGS */
static int lines_reached; /* highest line with data + 1: how much of the sheet is on screen */
static float sheet_y = LCD_ROWS; /* top of the sheet; it slides in from the bottom edge */
static float bar_y; /* top-bar vertical offset: 0 down/visible, -BAR_H slid up/hidden */
static unsigned last_data_ms;
static char current_name[NAME_CAP] = "untitled";
static bool started; /* START meta received, transmission in progress */
static unsigned hearing_ms; /* last time the demodulator heard a tone (before a pass begins) */
static void begin_pass(unsigned now); /* start the pass on the first decoded frame; defined near on_meta */
static void start_mic(void);
static void stop_mic(void);
static unsigned started_ms; /* when the START block ended: the pass timeline starts here */
static bool rx_paused; /* transmitter paused: freeze the timeline until RESUME */
static unsigned pause_anchor; /* the timeline clock is held here while paused */
static char note_buf[SSTV_NOTE_MAX_CHARS + 1]; /* assembled note text (note passes) */
static int total_frames; /* physical frames in the pass, announced in START */
static bool image_dirty; /* something received since the last save */
static bool finished; /* END meta received */
static unsigned finished_ms;
static int finished_lines;
static int mic_state; /* 0 asking, 1 allowed, -1 denied */
static bool mic_running; /* callback installed */
static bool show_panel; /* side panel with statistics, toggled with Right */
static bool confirm_leave; /* B during an active pass asks before leaving Receive */
static bool preview_dirty; /* a preview/tiny chunk arrived: refresh once per frame, not per chunk */
static bool confirm_delete; /* full view: A asks before deleting the image */
static unsigned full_ui_ms; /* full view: show name+hints for 3 s after entry or any press */
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
static uint8_t gallery_kind[MAX_GALLERY]; /* 0 = image (.pbm), 1 = note (.txt) */
static bool full_is_note; /* the item open in full view is a note */
static char full_note_text[SSTV_NOTE_MAX_CHARS + 1];
static bool note_dirty; /* a note chunk arrived since the last save */
static LCDBitmap* menu_thumb; /* latest saved image, for the gallery tile */
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define THUMB_W 100
#define THUMB_H 60
#define THUMB_R 8

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream (Data/recN.wav, first free N). */
#define REC_SECONDS 30
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */
static const char* rec_prefix = "rec"; /* "rec" (Up) or "test" (Down, tone test takes) */

/* ---- audio thread ---- */

static int mic_cb(void* context, int16_t* data, int len) {
    (void)context;
    int stored = mic_ring_write(&ring, data, len);
    if(stored < len) mic_dropped += (unsigned)(len - stored);
    return 1;
}

/* ---- names ---- */

/* True when the incoming meta carried no usable name (empty or all control/space). */
static bool name_is_blank(const char* s) {
    for(; *s; s++)
        if((unsigned char)*s > ' ') return false;
    return true;
}

/* First free untitled_N (no extension) so an unnamed pass never overwrites another file. */
static void next_untitled(char* out, size_t cap) {
    FileStat st;
    for(int n = 1; n < 10000; n++) {
        char probe[NAME_CAP];
        snprintf(probe, sizeof probe, "untitled_%d.pbm", n);
        if(pd->file->stat(probe, &st) != 0) { /* not found -> free */
            snprintf(out, cap, "untitled_%d", n);
            return;
        }
    }
    strncpy(out, "untitled", cap - 1);
    out[cap - 1] = 0;
}

/* Display form: file name without the .pbm extension. */
static void display_name(char* out, size_t cap, const char* in) {
    strncpy(out, in, cap - 1);
    out[cap - 1] = 0;
    size_t n = strlen(out);
    if(n > 4 && (strcmp(out + n - 4, ".pbm") == 0 || strcmp(out + n - 4, ".txt") == 0)) out[n - 4] = 0;
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

/* Saves the received note as a .txt (first line = the label). Unique untitled_N if unnamed. */
static void save_note(void) {
    if(!note_dirty) return;
    /* trim trailing NULs already handled by the buffer being zeroed; find the length */
    char base[NAME_CAP];
    if(name_is_blank(current_name)) next_untitled(base, sizeof base);
    else display_name(base, sizeof base, current_name);
    char fname[NAME_CAP + 8];
    snprintf(fname, sizeof fname, "%s.txt", base);
    SDFile* f = pd->file->open(fname, kFileWrite);
    if(!f) return;
    pd->file->write(f, note_buf, (unsigned)strlen(note_buf));
    pd->file->close(f);
    note_dirty = false;
}

/* Reads a saved note .txt into out (NUL-terminated). */
static bool read_note(const char* name, char* out, int cap) {
    SDFile* f = pd->file->open(name, kFileReadData);
    if(!f) return false;
    int n = pd->file->read(f, out, cap - 1);
    pd->file->close(f);
    if(n < 0) n = 0;
    out[n] = 0;
    return true;
}

/* Persists whatever the current reception produced (a picture or a note). */
static void save_received(void) {
    if(rx.note) save_note();
    else save_image();
}

/* ---- gallery data ---- */

static void gallery_list_cb(const char* path, void* userdata) {
    (void)userdata;
    size_t n = strlen(path);
    if(n < 5 || gallery_count >= MAX_GALLERY) return;
    bool pbm = strcmp(path + n - 4, ".pbm") == 0;
    bool txt = strcmp(path + n - 4, ".txt") == 0;
    if(!pbm && !txt) return;
    strncpy(gallery_names[gallery_count], path, NAME_CAP - 1);
    gallery_names[gallery_count][NAME_CAP - 1] = 0;
    gallery_kind[gallery_count] = txt ? 1 : 0;
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

/* Renders a note as a mini sticky-note thumbnail (same template, scaled down). */
static LCDBitmap* make_note_thumb(const char* text) {
    LCDBitmap* t = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorWhite);
    pd->graphics->pushContext(t);
    draw_sticky_note(pd, font_ui, 0, 0, THUMB_W, THUMB_H, text);
    pd->graphics->popContext();
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
    save_received();
    free_thumbs();
    gallery_count = 0;
    pd->file->listfiles(".", gallery_list_cb, NULL, 0);
}

static bool gallery_load_full(int idx) {
    if(idx < 0 || idx >= gallery_count) return false;
    if(gallery_kind[idx] == 1) {
        full_is_note = true;
        return read_note(gallery_names[idx], full_note_text, sizeof full_note_text);
    }
    full_is_note = false;
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
        if(gallery_kind[i] == 1) {
            char txt[SSTV_NOTE_MAX_CHARS + 1];
            if(read_note(gallery_names[i], txt, sizeof txt)) thumbs[i] = make_note_thumb(txt);
        } else if(read_pbm(gallery_names[i], px)) {
            thumbs[i] = make_thumb(px);
        }
    }
    pd->system->realloc(px, 0);
}

static void gallery_open(void) {
    stop_mic();
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
        int last = gallery_count - 1;
        if(gallery_kind[last] == 1) {
            char txt[SSTV_NOTE_MAX_CHARS + 1];
            if(read_note(gallery_names[last], txt, sizeof txt)) menu_thumb = make_note_thumb(txt);
        } else {
            uint8_t* px = pd->system->realloc(NULL, SSTV_IMAGE_BYTES);
            if(read_pbm(gallery_names[last], px)) menu_thumb = make_thumb(px);
            pd->system->realloc(px, 0);
        }
    }
    ui_mode = ModeMenu;
    confirm_leave = false;
    confirm_delete = false;
    stop_mic();
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
    begin_pass(pd->system->getCurrentTimeMilliseconds());
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
    preview_dirty = true;
    last_data_ms = pd->system->getCurrentTimeMilliseconds();
}

static void on_pixels(void* ctx, uint16_t line, int x_byte, int nbytes, const uint8_t* pixels) {
    (void)ctx;
    begin_pass(pd->system->getCurrentTimeMilliseconds());
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
    memset(note_buf, 0, sizeof note_buf); /* rx.note itself is owned by the START/END flag */
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
    bar_y = 0;
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
    save_received();
}

/* Lines the transmitter has sent by now according to the announced timeline: the START block
 * ended at started_ms, then the preview frames (and, coded, the schedule block), then the data
 * frames, then the END block. Raw passes carry two lines per block; coded passes follow the
 * schedule (lines per block) where it arrived and spread lines evenly where it did not.
 * Drives the sheet so lost frames do not stall it. */
static int expected_lines(unsigned now) {
    if(!started || total_frames <= 0) return 0;
    int intro = rx.coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES;
    int pass_data = 0; /* data positions in the pass (START + intro + data) */
    for(int d = 1; d <= SSTV_MAX_DATA_FRAMES; d++) {
        if(sstv_pass_total(d, sstv_pass_intro(rx.coded)) == total_frames) {
            pass_data = d;
            break;
        }
    }
    if(pass_data == 0) pass_data = total_frames - SSTV_ILEAVE; /* unknown layout: treat all as data */
    int data_frames = pass_data - SSTV_ILEAVE - intro;
    if(data_frames <= 0) return 0;
    /* physical frames elapsed since START ended -> data frames sent (parity blocks stand still) */
    int phys = SSTV_ILEAVE + (int)((float)(now - started_ms) * 1000.0f / (float)SSTV_PHYS_US);
    float frames = (float)(sstv_pass_data_before(phys, pass_data, sstv_pass_intro(rx.coded)) - SSTV_ILEAVE - intro);
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

/* Marks a transmission in progress on the first decoded frame of a pass, so the intro scene
 * and its preview show even when the START frame itself was lost (START only adds the name and
 * the timeline). Clears once per pass; a later START still forces a clean restart. */
static void begin_pass(unsigned now) {
    if(started) return;
    clear_image();
    started = true;
    started_ms = now;
    last_data_ms = now;
    ui_mode = ModeReceive;
}

static void on_meta(void* ctx, const char* name, bool is_end, int frames, bool coded) {
    (void)ctx; (void)coded;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(is_end) {
        finish(now); /* the transmitter is done: save and announce */
        return;
    }
    /* A new transmission starts: keep what the previous one produced, then start fresh. */
    save_received();
    clear_image();
    started = true;
    started_ms = now;
    total_frames = frames;
    if(name_is_blank(name)) {
        next_untitled(current_name, NAME_CAP); /* couldn't read the name: make a fresh unique one */
    } else {
        strncpy(current_name, name, NAME_CAP - 1);
        current_name[NAME_CAP - 1] = 0;
    }
    last_data_ms = now;
    /* Whatever screen is up, an incoming picture takes over. */
    ui_mode = ModeReceive;
}

static void on_control(void* ctx, int subtype, uint32_t arg) {
    (void)ctx;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(subtype == SSTV_META_PAUSE) {
        if(!rx_paused) {
            rx_paused = true;
            pause_anchor = now; /* hold the animation clock here */
        }
    } else if(subtype == SSTV_META_RESUME) {
        if(rx_paused) {
            rx_paused = false;
            int n = (int)arg;
            if(n < SSTV_ILEAVE) n = SSTV_ILEAVE;
            /* set the origin so elapsed maps to physical frame n right now */
            started_ms = now - (unsigned)((uint64_t)(n - SSTV_ILEAVE) * SSTV_PHYS_US / 1000);
            last_data_ms = now;
        }
    }
}

static void on_note(void* ctx, int chunk, int nbytes, const uint8_t* text) {
    (void)ctx;
    int off = chunk * SSTV_NOTE_CHUNK;
    for(int k = 0; k < nbytes && off + k < SSTV_NOTE_MAX_CHARS; k++) note_buf[off + k] = (char)text[k];
    note_dirty = true;
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    (void)ctx;
    led_frame_ms = pd->system->getCurrentTimeMilliseconds();
    int before = rx.blocks_ok;
    sstv_rx_phys(&rx, frame, soft);
    if(rx.blocks_ok != before) { led_block_ms = led_frame_ms; last_block_type = rx.last_type; last_block_ms = led_frame_ms; }
}

static void start_mic(void) {
    /* The microphone runs only in Receive; the menu and gallery keep it off. */
    if(mic_state != 1 || mic_running || ui_mode != ModeReceive) return;
    pd->sound->setMicCallback(mic_cb, NULL, kMicInputAutodetect);
    mic_running = true;
    pd->system->setAutoLockDisabled(1); /* a pass has no button input for minutes; stay awake */
}

static void stop_mic(void) {
    if(!mic_running) return;
    pd->sound->setMicCallback(NULL, NULL, kMicInputAutodetect);
    mic_running = false;
    pd->system->setAutoLockDisabled(0);
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
    /* First free name: rec1.wav, rec2.wav, ... so several takes can be fetched together. */
    char path[16];
    FileStat st;
    int n = 1;
    do {
        snprintf(path, sizeof path, "%s%d.wav", rec_prefix, n++);
    } while(n < 100 && pd->file->stat(path, &st) == 0);
    SDFile* f = pd->file->open(path, kFileWrite);
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
    return pd->graphics->getTextWidth(cur_font, text, strlen(text), kUTF8Encoding, 0);
}

static int font_height(void) {
    return cur_font ? pd->graphics->getFontHeight(cur_font) : 16;
}

static void draw_text(const char* text, int x, int y) {
    pd->graphics->drawText(text, strlen(text), kUTF8Encoding, x, y);
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

/* Integer upscale of a 1-bit sprite (black = ink) into a transparent bitmap: every ink pixel a
 * dot x dot square on a `pitch` grid (pitch == dot: solid). */
static LCDBitmap* render_scaled(LCDBitmap* src, int pitch, int dot) {
    int w, h, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(src, &w, &h, &rb, NULL, &data);
    LCDBitmap* out = pd->graphics->newBitmap(w * pitch, h * pitch, kColorClear);
    pd->graphics->pushContext(out);
    for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++)
            if(!(data[y * rb + (x >> 3)] & (0x80 >> (x & 7)))) /* 0 = black = ink */
                pd->graphics->fillRect(x * pitch, y * pitch, dot, dot, kColorBlack);
    pd->graphics->popContext();
    return out;
}

static void draw_led(void) {
    /* Virtual LED in the top-right corner: filled on every received frame, ring when a block
     * decodes, outline otherwise. Always visible. */
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    int cx = LCD_COLUMNS - 16, cy = BAR_H / 2; /* centred vertically in the top bar */
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
    pd->graphics->fillRect(0, 0, LCD_COLUMNS, BAR_H, kColorBlack);
    use_font(font_glyph); /* one font for the title and the hint glyphs */
    int ty = (BAR_H - font_height()) / 2;
    draw_text_white(title, 10, ty);
    if(hint) draw_text_white(hint, LCD_COLUMNS - 10 - text_width(hint), ty);
}

/* The Playdate silhouette at (x, y). Its screen is dark when off; while a picture is coming in
 * the screen lights up and shows `screen` (80x48) centred in the 88x53 interior. */
#define CONSOLE_W 140
#define CONSOLE_H 118
#define CONSOLE_SCREEN_X 7
#define CONSOLE_SCREEN_Y 7
#define CONSOLE_SCREEN_W 88
#define CONSOLE_SCREEN_H 53

static void draw_console(int x, int y, LCDBitmap* screen) {
    if(console) pd->graphics->drawBitmap(console, x, y, kBitmapUnflipped);
    else pd->graphics->drawRoundRect(x, y, CONSOLE_W, CONSOLE_H, 10, 2, kColorBlack);
    if(screen) {
        pd->graphics->fillRect(x + CONSOLE_SCREEN_X, y + CONSOLE_SCREEN_Y, CONSOLE_SCREEN_W, CONSOLE_SCREEN_H, kColorWhite);
        pd->graphics->drawBitmap(screen, x + CONSOLE_SCREEN_X + (CONSOLE_SCREEN_W - SSTV_PREVIEW_W) / 2,
                                 y + CONSOLE_SCREEN_Y + (CONSOLE_SCREEN_H - SSTV_PREVIEW_H) / 2, kBitmapUnflipped);
    }
}

/* Water arc from the dolphin's blowhole to the console screen; `t` drives the droplets. The
 * arc stays low enough (40 px) to remain on screen. */
static void draw_spout(int x0, int y0, int x1, int y1, float t) {
    const int drops = 9;
    for(int k = 0; k < drops; k++) {
        float u = fmodf(t + (float)k / (float)drops, 1.0f);
        float x = (float)x0 + ((float)(x1 - x0)) * u;
        float y = (float)y0 + ((float)(y1 - y0)) * u - 40.0f * sinf(3.14159f * u);
        int r = 2 + (int)(2.0f * sinf(3.14159f * u));
        pd->graphics->fillEllipse((int)x - r, (int)y - r, 2 * r, 2 * r, 0.0f, 0.0f, kColorBlack);
    }
}

/* ---- screens ---- */

static void draw_menu(void) {
    pd->graphics->clear(kColorWhite);
    /* "RX" pill exactly as tall as the title's capitals (Roobert 20: rows 4..23 of the line;
     * Roobert 10 Bold capitals: rows 2..11), then the title */
    const int title_y = 8, cap_top = title_y + 4, cap_h = 20;
    use_font(font_ui);
    int pill_w = text_width("RX") + 14;
    pd->graphics->fillRoundRect(16, cap_top, pill_w, cap_h, cap_h / 2, kColorBlack);
    draw_text_white("RX", 16 + 7, cap_top + (cap_h - 10) / 2 - 2);
    use_font(font_big);
    draw_text("Slow Flipper TV", 16 + pill_w + 8, title_y);

    /* two tiles: an icon band and a label band under a separator */
    const int tile_w = 190, tile_h = 186, ty = 46, label_h = 40;
    const int tx[2] = {5, LCD_COLUMNS - 5 - tile_w};
    const char* labels[2] = {"Receive", "Gallery"};
    for(int i = 0; i < 2; i++) {
        bool sel = i == menu_index;
        int bw = sel ? 3 : 1; /* border */
        pd->graphics->fillRoundRect(tx[i], ty, tile_w, tile_h, 14, kColorBlack);
        pd->graphics->fillRoundRect(tx[i] + bw, ty + bw, tile_w - 2 * bw, tile_h - 2 * bw, 14 - bw, kColorWhite);
        int band_h = tile_h - label_h; /* icon band height */
        if(i == 0) {
            if(dolphin_tile) {
                int iw, ih, irb;
                uint8_t* idata;
                pd->graphics->getBitmapData(dolphin_tile, &iw, &ih, &irb, NULL, &idata);
                pd->graphics->drawBitmap(dolphin_tile, tx[i] + (tile_w - iw) / 2, ty + (band_h - ih) / 2 + 2, kBitmapUnflipped);
            }
        } else {
            /* stack of pictures, the latest on top */
            int sx = tx[i] + (tile_w - THUMB_W) / 2 - 6, sy = ty + (band_h - THUMB_H) / 2 - 6;
            for(int k = 2; k >= 1; k--) {
                pd->graphics->fillRoundRect(sx + k * 6, sy + k * 6, THUMB_W, THUMB_H, THUMB_R, kColorWhite);
                pd->graphics->drawRoundRect(sx + k * 6, sy + k * 6, THUMB_W, THUMB_H, THUMB_R, 1, kColorBlack);
            }
            pd->graphics->fillRoundRect(sx, sy, THUMB_W, THUMB_H, THUMB_R, kColorWhite);
            if(menu_thumb) pd->graphics->drawBitmap(menu_thumb, sx, sy, kBitmapUnflipped);
            pd->graphics->drawRoundRect(sx, sy, THUMB_W, THUMB_H, THUMB_R, 1, kColorBlack);
        }
        /* label band */
        int ly = ty + band_h;
        pd->graphics->drawLine(tx[i] + bw, ly, tx[i] + tile_w - bw, ly, 1, kColorBlack);
        use_font(font_big);
        if(sel) {
            pd->graphics->fillRect(tx[i] + bw, ly + 1, tile_w - 2 * bw, label_h - bw - 1, kColorBlack);
            pd->graphics->fillRoundRect(tx[i] + bw, ly - 8, tile_w - 2 * bw, label_h + 8 - bw, 14 - bw, kColorBlack);
            pd->graphics->fillRect(tx[i] + bw, ly - 8, tile_w - 2 * bw, 8, kColorWhite);
            pd->graphics->drawLine(tx[i] + bw, ly, tx[i] + tile_w - bw, ly, 1, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
            draw_text_centered(labels[i], tx[i] + tile_w / 2, ly + (label_h - 20) / 2 - 4);
            pd->graphics->setDrawMode(kDrawModeCopy);
        } else {
            draw_text_centered(labels[i], tx[i] + tile_w / 2, ly + (label_h - 20) / 2 - 4);
        }
    }
    use_font(font_ui);
    if(gallery_count > 0) {
        char t[32];
        snprintf(t, sizeof t, "%d saved", gallery_count);
        draw_pill(t, tx[1] + tile_w - 12 - text_width(t) - 12, ty + 10, false);
    }
}

/* The receive-scene dolphin pose for the current state: idle, signal heard, START/preview,
 * picture data, or outer-code repair. */
static LCDBitmap* scene_dolphin(unsigned now) {
    if(!started) return (now - hearing_ms < 500 && fr_signal) ? fr_signal : fr_idle;
    if(now - last_block_ms < 800) {
        if(last_block_type == SSTV_FT_PARITY) return fr_repair;
        if(last_block_type == SSTV_FT_RAW || last_block_type == SSTV_FT_CODED) return fr_data;
        return fr_preview; /* META: START, tiny, preview, schedule */
    }
    return fr_data;
}

/* Intro scene: dolphin sends the preview to a Playdate. Shown while listening and until the
 * first picture lines arrive. */
static void draw_scene(void) {
    unsigned now = rx_paused ? pause_anchor : pd->system->getCurrentTimeMilliseconds();
    pd->graphics->clear(kColorWhite);
    char t[64];
    /* dolphin (2x) and console (140x118) below the top bar (drawn by draw_receive) */
    const int cx = 240, cy = 60; /* console (lowered under the bar, centred over the text) */
    const int dx = 20, dy = cy + (CONSOLE_H - DOLPHIN_H * 2) / 2; /* dolphin */
    const int text_cx = LCD_COLUMNS / 2; /* texts run under both figures */
    LCDBitmap* dfin = scene_dolphin(now);
    if(dfin) pd->graphics->drawBitmap(dfin, dx, dy, kBitmapUnflipped);
    else if(dolphin_tile) pd->graphics->drawBitmap(dolphin_tile, dx, dy, kBitmapUnflipped);
    draw_console(cx, cy, started ? preview_bitmap : NULL);
    use_font(font_ui);
    if(started) {
        draw_spout(dx + 70, dy + 2, cx + CONSOLE_SCREEN_X + CONSOLE_SCREEN_W / 2, cy + CONSOLE_SCREEN_Y + CONSOLE_SCREEN_H / 2, (float)(now % 1200) / 1200.0f);
        if(segs_done == 0) {
            /* Preview + intro repair progress: the bar spans everything before the picture
             * (tiny + preview, the schedule for coded passes, and the intro group's parity), so
             * it reaches 100% right before the picture starts. It advances by elapsed time, and
             * while the preview chunks are still arriving it is clamped between the decoded count
             * and one 4-frame block ahead, so a lost block holds it; the parity tail runs on time. */
            int intro_total = SSTV_INTRO_FRAMES + (rx.coded ? SSTV_SCHEDULE_CHUNKS : 0) + SSTV_OUTER_PARITY;
            float est = (float)(now - started_ms) / (float)(SSTV_PHYS_US / 1000);
            if(preview_chunks < SSTV_INTRO_FRAMES) {
                if(est < (float)preview_chunks) est = (float)preview_chunks;
                if(est > (float)(preview_chunks + SSTV_ILEAVE)) est = (float)(preview_chunks + SSTV_ILEAVE);
            }
            if(est > (float)intro_total) est = (float)intro_total;
            if(est < 0.0f) est = 0.0f;
            float frac = est / (float)intro_total;
            int ty = 200;
            draw_text_centered("Receiving preview", text_cx, ty);
            int bw = 220, bx = text_cx - bw / 2, by = ty + font_height() + 8;
            pd->graphics->drawRoundRect(bx, by, bw, 10, 5, 1, kColorBlack);
            int fill = (int)((float)(bw - 4) * frac);
            if(fill > 0) pd->graphics->fillRoundRect(bx + 2, by + 2, fill, 6, 3, kColorBlack);
        }
    } else {
        use_font(font_ui);
        const char* hint = mic_state < 0 ? "microphone access denied" : mic_state == 0 ? "waiting for microphone permission" : "start a transmission on the Flipper";
        draw_text_centered(hint, text_cx, 208);
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
    bool quiet = segs_done > 0 && !finished && !rx_paused && now - last_data_ms > LOST_TIMEOUT_MS;
    use_font(font_ui);
    int lh = font_height() + 3;

    display_name(nm, sizeof nm, current_name);
    fit_text(t, sizeof t, nm, PANEL_W - 16 - 20);
    draw_text(t, x, y);
    y += lh + 4;
    /* state and mode pills */
    const char* state = rx_paused ? "PAUSE" : finished ? "DONE" : segs_done == SSTV_NUM_SEGS ? "DONE" : quiet ? "LOST" : segs_done > 0 || started ? "RX" : "IDLE";
    int px = x;
    px += draw_pill(state, px, y, true) + 5;
    px += draw_pill(rx.coded ? "ZIP" : "RAW", px, y, false) + 5;
    if(rec_len >= 0) draw_pill(rec_prefix[0] == 't' ? "TEST" : "REC", px, y, true);
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
    /* counters: label left, value right; ok/bad and rs/repaired combined into one line each */
    char v0[16], v1[16], v2[16], v3[16], v4[16], v5[16];
    snprintf(v0, sizeof v0, "%d", rx.frames_in);
    snprintf(v1, sizeof v1, "%d", preview_chunks);
    snprintf(v2, sizeof v2, "%d/%d", rx.frames_ok, rx.frames_bad);
    snprintf(v3, sizeof v3, "%d/%d", rx.rs_corrected, rx.frames_repaired);
    snprintf(v4, sizeof v4, "%d", demod.flywheel_frames);
    snprintf(v5, sizeof v5, "%d", (int)mic_dropped);
    struct { const char* k; const char* v; } rows[] = {
        {"recv", v0}, {"preview", v1}, {"frames ok/bad", v2}, {"rs fixed/fixed", v3},
        {"flywheel", v4}, {"dropped", v5},
    };
    for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        use_font(font_ui);
        draw_text(rows[i].k, x, y);
        use_font(font_num);
        draw_text(rows[i].v, right - text_width(rows[i].v), y + 1);
        y += lh;
    }
    /* tone: a three-lamp traffic light for data tones 1/2/3 (the active one filled) */
    y += 6;
    use_font(font_ui);
    draw_text("tone", x, y);
    int r = 7, gap = 8, cx0 = x + 44, ccy = y + font_height() / 2;
    for(int k = 0; k < 3; k++) {
        int ccx = cx0 + k * (2 * r + gap);
        if(st.tone == k + 1) pd->graphics->fillEllipse(ccx, ccy - r, 2 * r, 2 * r, 0.0f, 0.0f, kColorBlack);
        else pd->graphics->drawEllipse(ccx, ccy - r, 2 * r, 2 * r, 1, 0.0f, 0.0f, kColorBlack);
    }
    /* hints */
    pd->graphics->drawLine(x0, LCD_ROWS - 20, LCD_COLUMNS, LCD_ROWS - 20, 1, kColorBlack);
    use_font(font_glyph);
    draw_text("Ⓐ clear   Ⓑ menu", x, LCD_ROWS - 16);
}

/* Banner over the picture for a few seconds after the END frame. */
static void draw_finished_banner(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(!finished || now - finished_ms > 6000) return;
    char t[80], nm[NAME_CAP];
    display_name(nm, sizeof nm, current_name);
    use_font(font_ui);
    snprintf(t, sizeof t, "Received \"%s\"   %d/%d lines", nm, finished_lines, SSTV_HEIGHT);
    int w = text_width(t) + 24, h = font_height() + 16;
    int x = (LCD_COLUMNS - w) / 2, y = LCD_ROWS - h - 14;
    pd->graphics->fillRoundRect(x, y, w, h, h / 2, kColorBlack);
    draw_text_white(t, x + 12, y + 8);
}

/* The picture is a sheet that crawls in from the bottom edge as lines arrive (the Flipper
 * shows the same sheet leaving through its top edge). It covers the intro scene as it rises. */
static void draw_receive(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    unsigned enow = rx_paused ? pause_anchor : now; /* the timeline stands still while paused */
    int y, frontier;
    if(rx.note) {
        /* a note pass: the sticky note slides out smoothly as its frames arrive, then rests */
        const int rest = BAR_H + 6;
        float frac = 0.0f;
        if(finished) {
            frac = 1.0f;
        } else if(started) {
            int textframes = rx.data_frames > SSTV_ILEAVE ? rx.data_frames - SSTV_ILEAVE : SSTV_NOTE_MAX_FRAMES;
            int elapsed = (int)((float)(enow - started_ms) * 1000.0f / (float)SSTV_PHYS_US);
            frac = textframes > 0 ? (float)elapsed / (float)textframes : 1.0f;
            if(frac < 0.0f) frac = 0.0f;
            if(frac > 1.0f) frac = 1.0f;
        }
        float ntarget = (float)LCD_ROWS - frac * (float)(LCD_ROWS - rest);
        float d = ntarget - sheet_y;
        if(d < -0.5f || d > 0.5f) sheet_y += d * 0.15f;
        else sheet_y = ntarget;
        y = (int)(sheet_y + 0.5f);
        frontier = y; /* used by the bar slide below */
        if(y > 0) draw_scene();
        else pd->graphics->clear(kColorWhite);
        int nx = 22, nw = LCD_COLUMNS - 44, nh = LCD_ROWS - y - 12;
        if(nh > 24) draw_sticky_note(pd, font_ui, nx, y, nw, nh, note_buf);
        goto chrome; /* skip the picture-sheet code */
    }
    int shown = expected_lines(enow);
    if(lines_reached > shown) shown = lines_reached; /* never hide a line that arrived */
    /* The drawing frontier (the newest line) leads SHEET_LEAD px above the bottom edge, so you
     * watch lines land there over a semi-transparent preview placeholder in the band below.
     * The image top climbs until it hits the screen top (SHEET_LEAD lines before the end); the
     * frontier then walks down through the band, filling the last lines over the placeholder. */
    int target;
    if(shown <= 0) target = LCD_ROWS; /* nothing yet: scene only */
    else if(shown <= LCD_ROWS - SHEET_LEAD) target = LCD_ROWS - SHEET_LEAD - shown; /* rising */
    else target = 0; /* image aligned to the top; the frontier now descends into the band */
    float d = (float)target - sheet_y;
    if(d < -0.5f || d > 0.5f) sheet_y += d * 0.15f;
    else sheet_y = (float)target;
    y = (int)(sheet_y + 0.5f); /* screen y of image row 0 (the sheet's top) */
    frontier = y + shown; /* screen y of the newest drawn line */
    if(frontier > LCD_ROWS) frontier = LCD_ROWS;
    if(y > 0) draw_scene(); /* listening, or START + preview received; the sheet grows over it */
    else pd->graphics->clear(kColorWhite);
    if(y < LCD_ROWS) {
        /* the full picture bitmap already carries the checker preview placeholder for lines
         * that have not arrived, so the band below the frontier shows the preview */
        pd->graphics->drawBitmap(image, 0, y, kBitmapUnflipped);
        if(frontier > 0 && frontier < LCD_ROWS)
            pd->graphics->fillRect(0, frontier - 2, LCD_COLUMNS, 2, kColorBlack); /* drawing frontier */
    }
chrome:;
    /* The bar stays on top so the picture slides in from under it. As the picture's top edge
     * nears the bar, the bar slides up and off with its own smooth animation (not the sheet's
     * speed), uncovering the top of the image. */
    float bar_target = (y < BAR_H + 40) ? -(float)BAR_H : 0.0f;
    float bd = bar_target - bar_y;
    if(bd < -0.5f || bd > 0.5f) bar_y += bd * 0.18f; else bar_y = bar_target;
    int boff = (int)(bar_y - 0.5f);
    if(boff > -BAR_H) { /* still at least partly on screen */
        pd->graphics->setDrawOffset(0, boff);
        if(started) {
            char nm[NAME_CAP], bar[NAME_CAP + 16];
            display_name(nm, sizeof nm, current_name);
            snprintf(bar, sizeof bar, "Receiving: \"%s\"", nm);
            draw_title_bar(bar, NULL);
        } else {
            draw_title_bar("Listening", NULL);
        }
        pd->graphics->setDrawOffset(0, 0);
    }
    /* Picture fully in but the pass is not done yet: the outer-code parity is still arriving.
     * Show a spinner and "Verifying" until END (or the timeout) finishes the pass. */
    if(started && !finished && !rx_paused && !rx.note && frontier >= LCD_ROWS) {
        use_font(font_ui);
        const char* v = "Verifying";
        int r = 6, sp = 2 * r, pad = 10, gap = 6, tw = text_width(v);
        int w = pad + sp + gap + tw + pad, h = 24, bx = 10, by = 10;
        pd->graphics->fillRoundRect(bx, by, w, h, h / 2, kColorBlack);
        float a0 = (float)(now % 900) / 900.0f * 360.0f;
        int scx = bx + pad, scy = by + (h - sp) / 2;
        pd->graphics->drawEllipse(scx, scy, sp, sp, 2, a0, a0 + 270.0f, kColorWhite);
        draw_text_white(v, bx + pad + sp + gap, by + (h - font_height()) / 2);
    }
    if(rx_paused) {
        use_font(font_ui);
        const char* pm = "Paused - waiting for sender";
        int tw = text_width(pm), w = tw + 28, h = 28;
        int bx = (LCD_COLUMNS - w) / 2, by = 12;
        pd->graphics->fillRoundRect(bx, by, w, h, h / 2, kColorBlack);
        draw_text_white(pm, bx + 14, by + (h - font_height()) / 2);
    }
    if(show_panel) draw_panel();
    draw_finished_banner();
    if(confirm_leave) {
        const char* msg = "Leave? Reception in progress";
        const char* keys = "Ⓐ leave    Ⓑ stay";
        use_font(font_ui);
        int w1 = text_width(msg);
        use_font(font_glyph);
        int w2 = text_width(keys);
        int w = (w1 > w2 ? w1 : w2) + 24, bx = (LCD_COLUMNS - w) / 2, by = LCD_ROWS / 2 - 28;
        pd->graphics->fillRoundRect(bx, by, w, 52, 10, kColorWhite);
        pd->graphics->drawRoundRect(bx, by, w, 52, 10, 2, kColorBlack);
        use_font(font_ui);
        draw_text(msg, bx + 12, by + 8);
        use_font(font_glyph);
        draw_text(keys, bx + 12, by + 28);
    }
}

static void draw_grid(void) {
    pd->graphics->clear(kColorWhite);
    char t[48], nm[NAME_CAP];
    int pages = (gallery_count + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
    if(pages > 1)
        snprintf(t, sizeof t, "Gallery  %d  -  page %d/%d", gallery_count, gallery_page / GRID_PER_PAGE + 1, pages);
    else
        snprintf(t, sizeof t, "Gallery   %d image%s", gallery_count, gallery_count == 1 ? "" : "s");
    draw_title_bar(t, "Ⓐ open   Ⓑ back");
    use_font(font_ui);
    if(gallery_count == 0) {
        draw_text_centered("no saved images yet", LCD_COLUMNS / 2, 110);
        return;
    }
    int cell_w = LCD_COLUMNS / GRID_COLS, cell_h = (LCD_ROWS - BAR_H) / GRID_ROWS;
    gallery_ensure_thumbs(gallery_page, GRID_PER_PAGE);
    for(int k = 0; k < GRID_PER_PAGE; k++) {
        int idx = gallery_page + k;
        if(idx >= gallery_count) break;
        int cx = (k % GRID_COLS) * cell_w + (cell_w - THUMB_W) / 2;
        int cy = BAR_H + (k / GRID_COLS) * cell_h + 12;
        if(thumbs[idx]) pd->graphics->drawBitmap(thumbs[idx], cx, cy, kBitmapUnflipped);
        pd->graphics->drawRoundRect(cx - 1, cy - 1, THUMB_W + 2, THUMB_H + 2, THUMB_R, 1, kColorBlack);
        if(idx == gallery_index) pd->graphics->drawRoundRect(cx - 5, cy - 5, THUMB_W + 10, THUMB_H + 10, THUMB_R + 3, 3, kColorBlack);
        display_name(nm, sizeof nm, gallery_names[idx]);
        fit_text(t, sizeof t, nm, cell_w - 12);
        draw_text_centered(t, (k % GRID_COLS) * cell_w + cell_w / 2, cy + THUMB_H + 9);
    }
}

static void draw_full(void) {
    pd->graphics->clear(kColorWhite);
    if(full_is_note)
        draw_sticky_note(pd, font_ui, 24, 36, LCD_COLUMNS - 48, LCD_ROWS - 72, full_note_text); /* leave room for the top/bottom hint pills */
    else if(gallery_bitmap)
        pd->graphics->drawBitmap(gallery_bitmap, 0, 0, kBitmapUnflipped);
    char t[64], nm[NAME_CAP];
    /* name + hints show for 3 s after entering or any press, then hide for a clean full view */
    if(pd->system->getCurrentTimeMilliseconds() - full_ui_ms < 3000) {
        display_name(nm, sizeof nm, gallery_names[gallery_index]);
        use_font(font_ui);
        fit_text(t, sizeof t, nm, 200);
        draw_pill(t, 8, LCD_ROWS - font_height() - 12, true);
        use_font(font_glyph);
        const char* hint = "Ⓐ delete   Ⓑ back";
        draw_pill(hint, LCD_COLUMNS - 8 - (text_width(hint) + 12), 8, true);
    }
    if(confirm_delete) {
        const char* msg = full_is_note ? "Delete this note?" : "Delete this image?";
        const char* keys = "Ⓐ delete    Ⓑ keep";
        use_font(font_ui);
        int w1 = text_width(msg);
        use_font(font_glyph);
        int w2 = text_width(keys);
        int w = (w1 > w2 ? w1 : w2) + 24, bx = (LCD_COLUMNS - w) / 2, by = LCD_ROWS / 2 - 28;
        pd->graphics->fillRoundRect(bx, by, w, 52, 10, kColorWhite);
        pd->graphics->drawRoundRect(bx, by, w, 52, 10, 2, kColorBlack);
        use_font(font_ui);
        draw_text(msg, bx + 12, by + 8);
        use_font(font_glyph);
        draw_text(keys, bx + 12, by + 28);
    }
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
    if(preview_dirty) { /* coalesce a block's 4 chunks into one refresh so the scene does not hitch */
        preview_refresh_bitmap();
        for(int line = 0; line < SSTV_HEIGHT; line++) paint_placeholder_line(line);
        preview_dirty = false;
    }

    unsigned now = pd->system->getCurrentTimeMilliseconds();
    /* Before a pass begins, note when the demodulator is hearing a tone so the dolphin can
     * react (open its mouth) to a signal that is not yet a decoded transmission. */
    if(ui_mode == ModeReceive && !started) {
        sstv_demod_status_t st;
        sstv_demod_status(&demod, &st);
        /* tone >= 0 already means a detector is above the demodulator's noise floor (the
         * leader tones qualify); state != IDLE means it has locked onto the sync. */
        if(st.tone >= 0 || st.state != SSTV_DEMOD_IDLE) hearing_ms = now;
    }
    if(image_dirty && ((segs_done == SSTV_NUM_SEGS) || (now - last_data_ms > LOST_TIMEOUT_MS))) save_image();
    if(note_dirty && (now - last_data_ms > LOST_TIMEOUT_MS)) save_note();
    /* END never came: the pass is over by the announced timeline plus a margin. */
    if(started && total_frames > 0 && now - started_ms > (unsigned)(total_frames - SSTV_ILEAVE) * (SSTV_PHYS_US / 1000) + 4000) finish(now);

    switch(ui_mode) {
    case ModeMenu:
        if(pushed & (kButtonLeft | kButtonRight)) menu_index ^= 1;
        if(pushed & kButtonA) {
            if(menu_index == 0) {
                ui_mode = ModeReceive; /* the microphone runs only here */
                confirm_leave = false;
                clear_image(); /* each visit to Receive starts a fresh listen */
                start_mic();
            } else {
                gallery_open();
            }
        }
        draw_menu();
        break;
    case ModeReceive:
        if(confirm_leave) {
            if(pushed & kButtonA) { confirm_leave = false; menu_open(); }
            else if(pushed & kButtonB) confirm_leave = false;
            draw_receive();
            break;
        }
        if(pushed & kButtonA) clear_image();
        if(pushed & kButtonB) {
            if(started && !finished) confirm_leave = true; /* a pass is in progress: ask first */
            else menu_open();
        }
        if(pushed & kButtonRight) show_panel = !show_panel;
        if((pushed & kButtonUp) && rec_len < 0) {
            rec_prefix = "rec";
            rec_len = 0;
        }
        if((pushed & kButtonDown) && rec_len < 0) { /* temporary: tone test takes */
            rec_prefix = "test";
            rec_len = 0;
        }
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
            if((pushed & kButtonA) && gallery_load_full(gallery_index)) { ui_mode = ModeFull; full_ui_ms = now; }
        }
        if(ui_mode == ModeGrid) draw_grid();
        else draw_full();
        break;
    case ModeFull: {
        bool ui_visible = now - full_ui_ms < 3000; /* were the hints showing before this press? */
        if(pushed) full_ui_ms = now;              /* any press re-shows them */
        if(confirm_delete) {
            if(pushed & kButtonA) {
                if(gallery_index >= 0 && gallery_index < gallery_count)
                    pd->file->unlink(gallery_names[gallery_index], 0);
                confirm_delete = false;
                gallery_scan(); /* refresh the list and thumbnails */
                if(gallery_count == 0) { menu_open(); }
                else {
                    if(gallery_index >= gallery_count) gallery_index = gallery_count - 1;
                    if(!gallery_load_full(gallery_index)) ui_mode = ModeGrid;
                }
            } else if(pushed & kButtonB) {
                confirm_delete = false;
            }
            if(ui_mode == ModeFull) draw_full(); else draw_grid();
            break;
        }
        /* When the hints are hidden the first A/B just reveals them; the action runs only
         * while they are visible. Left/Right always navigate (and re-show). */
        if((pushed & kButtonA) && ui_visible) confirm_delete = true;
        if((pushed & kButtonB) && ui_visible) ui_mode = ModeGrid;
        if((pushed & kButtonRight) && gallery_count > 0) { gallery_index = (gallery_index + 1) % gallery_count; gallery_load_full(gallery_index); }
        if((pushed & kButtonLeft) && gallery_count > 0) { gallery_index = (gallery_index + gallery_count - 1) % gallery_count; gallery_load_full(gallery_index); }
        if(ui_mode == ModeFull) draw_full();
        else draw_grid();
        break;
    }
    }
    if(ui_mode == ModeReceive) draw_led(); /* the frame dot belongs to Receive, not the menu/gallery */
    return 1;
}

static void init(void) {
    const char* err = NULL;
    font_ui = pd->graphics->loadFont("fonts/Roobert-10-Bold", &err);
    font_num = pd->graphics->loadFont("fonts/Roobert-9-Mono-Condensed", &err);
    font_big = pd->graphics->loadFont("fonts/Roobert-20-Medium", &err);
    font_glyph = pd->graphics->loadFont("fonts/Roobert-11-Medium-extended", &err);
    if(!font_glyph) font_glyph = font_ui;
    if(!font_num) font_num = font_ui;
    if(!font_big) font_big = font_ui;
    dolphin_src = pd->graphics->loadBitmap("images/dolphin", &err);
    if(dolphin_src) dolphin_tile = render_scaled(dolphin_src, 2, 2);
    LCDBitmap* dolphin_open_src = pd->graphics->loadBitmap("images/dolphin_open", &err);
    if(dolphin_open_src) dolphin_open_tile = render_scaled(dolphin_open_src, 2, 2);
    /* 47x49 head poses, pixel-doubled to 94x98 (clean square pixels), beak facing the console */
    struct { LCDBitmap** dst; const char* path; } poses[] = {
        {&fr_idle, "images/frame_11"}, {&fr_signal, "images/frame_22"}, {&fr_preview, "images/frame_31"},
        {&fr_data, "images/frame_51"}, {&fr_repair, "images/frame_41"}};
    for(size_t i = 0; i < sizeof poses / sizeof poses[0]; i++) {
        LCDBitmap* src = pd->graphics->loadBitmap(poses[i].path, &err);
        if(src) { *poses[i].dst = render_scaled(src, 2, 2); pd->graphics->freeBitmap(src); }
    }
    console = pd->graphics->loadBitmap("images/console", &err);

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
    sstv_rx_set_control_cb(&rx, on_control);
    sstv_rx_set_note_cb(&rx, on_note);
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
            save_received();
            stop_mic();
        }
    } else if(event == kEventUnlock) {
        if(pd) start_mic();
    }
    return 0;
}
