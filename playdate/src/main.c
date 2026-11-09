#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <stdarg.h>

#include "pd_api.h"
#include "sstv_params.h"
#include "sstv_demod.h"
#include "sstv_rx.h"
#include "sstv_frame.h"
#include "mic_ring.h"
#include "sticky_note.h"
#include "sstv_anim.h"
#include "export.h"
#include "build_info.h"

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
static LCDFont* font_notes; /* ASCII + Cyrillic: note text (UTF-8) */
static LCDBitmap* dolphin_src; /* 71x49 outline dolphin (firmware L1_Furippa1 frame 0), faces right */
static LCDBitmap* dolphin_tile; /* menu tile and scene: 2x, 142x98 */
static LCDBitmap* dolphin_open_tile; /* legacy, kept for the menu tile fallback */
static LCDBitmap* fr_idle, *fr_signal, *fr_preview, *fr_data, *fr_repair; /* 128x49 receive-scene dolphin poses */
static int last_block_type = -1; static unsigned last_block_ms; /* drives the scene dolphin */
#define DOLPHIN_W 71
#define DOLPHIN_H 49
static LCDBitmap* console; /* 140x118 Playdate silhouette, screen interior at (7,7) 88x53 */
static LCDBitmap* thumb_mask; /* rounded corners for thumbnails */
static LCDBitmap* qr_github; /* 150x150: https://github.com/Negenii (the Flipper's code, 3 px per dot) */
static LCDBitmap* qr_coffee; /* 145x145: https://buymeacoffee.com/low.effort (tools/make_coffee_assets.py) */
static LCDBitmap* bmc_button; /* 200x56 Buy Me a Coffee button in 1 bit (tools/make_coffee_assets.py) */
static int about_page; /* which system-menu screen: 0 About, 1 Support */

static sstv_demod demod; /* tuned to the HF band */
static sstv_rx rx;
/* Near-ultrasound only (the audible band is archived: git tag audible-mode-archive). */
static const int active_band = SSTV_BAND_HF;
#define PHYS_US_NOW SSTV_BAND_PHYS_US(active_band) /* physical frame length of the band in use */
static mic_ring ring;

/* Receive state. Half-line segments (raw mode) or whole lines (coded mode) arrive verified. */
static uint8_t seg_seen[SSTV_HEIGHT]; /* bit 0: left half, bit 1: right half */
static int segs_done; /* of SSTV_NUM_SEGS */
static int lines_reached; /* highest line with data + 1: decoded lines available */
static float revealed; /* lines actually shown; eases up toward lines_reached, never past it */
static float sheet_y = LCD_ROWS; /* top of the sheet; it slides in from the bottom edge */
static float bar_y; /* top-bar vertical offset: 0 down/visible, -BAR_H slid up/hidden */
static unsigned last_data_ms;
static bool calib_active; /* calibration screen up (see calib_frame) */
/* End-of-pass duet (see chime_first_half in the Flipper app): after END the Playdate plays
 * the last two notes, rising (G6 C7) when every line arrived, falling (E-flat6 C6) when not.
 * The Flipper's C6 E6 take 300 ms from its last symbol; END decodes ~20-40 ms after that here
 * (mic buffering + one update), so the answer starts 270 ms after END. */
#define CHIME_DELAY_MS 270
#define CHIME_NOTE_S 0.15f
static PDSynth* chime_synth[2];
static unsigned chime_at; /* 0 = nothing pending */
/* Start-of-pass answer: the Flipper plays C6 G6 before its preamble; the Playdate answers C7 E7
 * on the first lock frame it decodes (~2 s in). Sine, not square: the speaker sits next to the
 * mic, and a square wave's odd harmonics land in the 17-21 kHz band ~20 dB under the note, far
 * louder than the received tones. The lock frames carry no data, so the overlap costs nothing. */
static PDSynth* start_synth[2];
static unsigned start_chime_ms;
static void play_start_chime(unsigned now);
static bool chime_ok;
static int calib_streak;
static char current_name[NAME_CAP] = "untitled";
static bool started; /* START meta received, transmission in progress */
static unsigned hearing_ms; /* last time the demodulator heard a tone (before a pass begins) */
static void begin_pass(unsigned now); /* start the pass on the first decoded frame; defined near on_meta */
static void start_mic(void);
static void stop_mic(void);
static unsigned started_ms; /* when the START block ended: the pass timeline starts here */
static bool rx_paused; /* transmitter paused: freeze the timeline until RESUME */
static unsigned pause_anchor; /* the timeline clock is held here while paused */
static char note_buf[SSTV_NOTE_MAX_BYTES + 1]; /* assembled note text (note passes, up to 64 KB) */
static int note_scroll; /* receive view scroll (px), crank-driven */
static bool note_follow = true; /* keep the newest text in view until the crank is used */
static int full_scroll; /* gallery note scroll (px) */
static int total_frames; /* physical frames in the pass, announced in START */
static bool image_dirty; /* something received since the last save */
static bool finished; /* END meta received */
static unsigned finished_ms;
static int finished_lines;
static int mic_state; /* 0 not asked yet, 2 the system dialog is up, 1 allowed, -1 denied */
static bool mic_running; /* callback installed */
static bool show_panel; /* side panel with statistics, toggled with Right */
static bool confirm_leave; /* B during an active pass asks before leaving Receive */
static bool preview_dirty; /* a preview/tiny chunk arrived: refresh once per frame, not per chunk */
static bool confirm_delete; /* full view: A asks before deleting the image */
static unsigned full_ui_ms; /* full view: show name+hints for 3 s after entry or any press */
static unsigned led_frame_ms; /* virtual LED: last physical frame time */
static unsigned led_block_ms; /* last decoded block time */
static volatile unsigned mic_dropped;
static float dsp_time; /* seconds spent decoding in the current ~1 s window */
static int dsp_samples, dsp_load; /* samples in the window; last load, % of real time */

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
static LCDBitmap* ph_bm; /* full-size preview placeholder, shown below the drawing line */

/* Screens. */
typedef enum { ModeMenu, ModeReceive, ModeGrid, ModeFull, ModeAbout } UiMode;
static UiMode ui_mode = ModeMenu;
static UiMode about_back; /* the screen About returns to */
static int menu_index; /* 0 receive, 1 gallery */
static char gallery_names[MAX_GALLERY][NAME_CAP];
static int gallery_count;
static int gallery_index; /* cursor */
static int gallery_page; /* first tile shown */
static LCDBitmap* gallery_bitmap;
static LCDBitmap* thumbs[MAX_GALLERY]; /* 100x60 thumbnails, loaded lazily */
static uint8_t gallery_kind[MAX_GALLERY]; /* 0 = image (.pbm), 1 = note (.txt), 2 = animation (.sfa) */
static bool full_is_note; /* the item open in full view is a note */
static char full_note_text[SSTV_NOTE_MAX_BYTES + 1];
static char thumb_txt[513]; /* a thumbnail only needs the start of a note */
static bool note_dirty; /* a note chunk arrived since the last save */
static LCDBitmap* menu_thumb; /* latest saved image, for the gallery tile */

/* Animation passes (revision 12): the stream sits in rx.data like note text; it is decoded as it
 * arrives and the frame being decoded is on screen, its pixels landing over its reference. */
static sstv_anim_info anim_info;
static int anim_hdr_len; /* 0 header not in yet, -1 not a valid stream */
static sstv_anim_dec anim_dec;
static uint8_t* anim_frames; /* n * frame bytes */
static bool anim_dirty; /* stream bytes arrived since the last save */
static LCDBitmap* anim_bm; /* one frame at its own size, drawn scaled */
static unsigned anim_play_ms;
static int anim_shown; /* frame on screen (title bar counter) */
static int anim_disp; /* frame being wiped in (see the receive view) */
static int anim_wipe_prev = -1; /* frame still showing below the wipe line, -1 none (white) */
static float anim_wipe_row; /* rows of anim_disp drawn so far */
static unsigned anim_wipe_ms;
static uint8_t* anim_compose; /* the frame on screen: new rows above the line, old below */
static uint16_t* anim_rowpos; /* stream position after each decoded row (anim_dec.row_pos) */
static float anim_line; /* drawing line, in rows across frames (frame * h + row) */
static bool anim_repairing; /* parity frames on the (delayed) air: show "Verifying" */
static int8_t anim_seg_ok[SSTV_ANIM_MAX_SEGS]; /* 1 CRC checked, -1 CRC mismatch, 0 bytes missing */
static int anim_rx_stamp = -1; /* receiver progress at the last segment check */
static unsigned anim_disp_ms;
static int anim_play_pos;
/* gallery full view of a saved animation */
static sstv_anim_info full_anim;
static uint8_t* full_anim_frames;
static bool full_is_anim;
#define GRID_COLS 3
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)
#define THUMB_W 100
#define THUMB_H 60
#define THUMB_R 8

/* Debug recording: Up starts a REC_SECONDS capture of the raw mic stream (Data/recN.wav, first free N). */
#define REC_SECONDS 60
#define REC_RATE 44100
static int16_t* rec_buf;
static int rec_len; /* samples captured so far, -1 when idle */
static const char* rec_prefix = "rec"; /* "rec" (Up) or "test" (Down, tone test takes) */
static char rec_saved[20]; /* name of the last saved recording, for the on-screen notice */
static unsigned rec_saved_ms;

/* ---- reception log: Data/<bundle>/rxlog.txt, read on a computer in disk mode. One line per
 * event, time in ms since launch first. Rotated to rxlog_old.txt past LOG_MAX bytes. ---- */
#define LOG_MAX (512 * 1024)
static SDFile* rx_logf;
static int log_bytes;
static unsigned log_flush_ms;

static void log_open(void) {
    FileStat st;
    if(pd->file->stat("rxlog.txt", &st) == 0 && st.size > LOG_MAX) {
        pd->file->unlink("rxlog_old.txt", 0);
        pd->file->rename("rxlog.txt", "rxlog_old.txt");
    }
    rx_logf = pd->file->open("rxlog.txt", kFileAppend);
}

static void rxlog(const char* fmt, ...) {
    if(!rx_logf) return;
    char b[240];
    int n = snprintf(b, sizeof b, "%u ", pd->system->getCurrentTimeMilliseconds());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if(n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    pd->file->write(rx_logf, b, (unsigned)n);
    log_bytes += n;
}

static void log_flush(void) {
    if(rx_logf) pd->file->flush(rx_logf);
}

/* Data positions of the pass that never arrived, as ranges ("4-7 120"), plus parity counts per
 * group: what the outer code had to work with. */
static void log_missing(const char* when) {
    int df = rx.data_frames;
    if(df <= 0 || df > SSTV_RX_MAX_DATA_FRAMES) {
        rxlog("%s: pass length unknown (data_frames %d)", when, df);
        return;
    }
    char b[200];
    int n = 0, miss = 0;
    for(int p = 0; p < df; p++) {
        if(rx.have[p]) continue;
        int q = p;
        while(q + 1 < df && !rx.have[q + 1]) q++;
        miss += q - p + 1;
        if(n < (int)sizeof b - 16) n += snprintf(b + n, sizeof b - (size_t)n, q > p ? "%d-%d " : "%d ", p, q);
        p = q;
    }
    b[n] = 0;
    rxlog("%s: missing %d/%d data positions: %s", when, miss, df, miss ? b : "none");
    int intro = rx.note ? SSTV_NOTE_INTRO(df) : sstv_pass_intro(rx.coded);
    int groups = sstv_pass_groups(df, intro);
    for(int g = 0; g < groups && g < SSTV_RX_GROUPS_MAX; g++) {
        int par = 0, have = 0, size = sstv_pass_group_size(df, intro, g), base = sstv_pass_group_base(intro, g);
        for(int j = 0; j < SSTV_OUTER_PARITY; j++) par += rx.have_parity[g][j];
        for(int p = 0; p < size; p++) have += rx.have[base + p];
        rxlog("%s: group %d data %d/%d parity %d/%d done %d", when, g, have, size, par, SSTV_OUTER_PARITY, rx.group_done[g]);
    }
}

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

/* First free name of the form <prefix>N (no extension) so an unnamed pass never overwrites. */
static void next_named(const char* prefix, const char* ext, char* out, size_t cap) {
    FileStat st;
    for(int n = 1; n < 10000; n++) {
        char probe[NAME_CAP];
        snprintf(probe, sizeof probe, "%s%d.%s", prefix, n, ext);
        if(pd->file->stat(probe, &st) != 0) {
            snprintf(out, cap, "%s%d", prefix, n);
            return;
        }
    }
    snprintf(out, cap, "%s", prefix);
}
static void next_note_name(char* out, size_t cap) {
    next_named("note_", "txt", out, cap);
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

static void file_write(void* ctx, const void* data, int len);
/* PNG row y: the picture where it arrived, white elsewhere (Playdate bitmap bits: 1 = white). */
static void png_row(void* ctx, int y, uint8_t* out) {
    (void)ctx;
    for(int i = 0; i < SSTV_LINE_BYTES; i++) {
        bool have = seg_seen[y] & (i < SSTV_SEG_BYTES ? 1 : 2);
        out[i] = have ? image_data[y * image_rowbytes + i] : 0xFF;
    }
}

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
    /* the same picture as a PNG next to it, for a computer (Data/<game>/ in disk mode) */
    char png[64];
    strncpy(png, fname, sizeof png - 1);
    png[sizeof png - 1] = 0;
    char* dot = strrchr(png, '.');
    if(dot) strcpy(dot, ".png");
    f = pd->file->open(png, kFileWrite);
    if(f) {
        export_png(SSTV_WIDTH, SSTV_HEIGHT, png_row, NULL, file_write, f);
        pd->file->close(f);
    }
    image_dirty = false;
}

/* Saves the received note as a .txt (first line = the label). Unique untitled_N if unnamed. */
static void save_note(void) {
    if(!note_dirty) return;
    /* trim trailing NULs already handled by the buffer being zeroed; find the length */
    char base[NAME_CAP];
    if(name_is_blank(current_name)) next_note_name(base, sizeof base);
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

/* ---- animations ---- */

static int rx_anim_byte(void* ctx, int pos) {
    (void)ctx;
    if(pos < 0 || pos >= rx.note_len) return -1;
    int at = SSTV_ILEAVE + pos / SSTV_NOTE_CHUNK;
    if(at >= SSTV_RX_MAX_DATA_FRAMES || !rx.have[at]) return -1;
    return rx.data[at][pos % SSTV_NOTE_CHUNK];
}

static void anim_reset(void) {
    anim_hdr_len = 0;
    anim_dirty = false;
    anim_play_pos = 0;
    anim_disp = 0;
    anim_disp_ms = 0;
    anim_wipe_prev = -1;
    anim_wipe_row = 0.0f;
    anim_wipe_ms = 0;
    if(anim_compose) anim_compose = pd->system->realloc(anim_compose, 0);
    anim_line = 0.0f;
    memset(anim_seg_ok, 0, sizeof anim_seg_ok);
    anim_rx_stamp = -1;
    if(anim_frames) anim_frames = pd->system->realloc(anim_frames, 0);
}

/* Parses the header once it is in, then decodes what has arrived (budget pixels; final: the
 * pass is over, decode everything that can be and give up on holes). */
static void anim_pump(int budget, bool final) {
    if(!rx.anim) return;
    if(anim_hdr_len == 0) {
        int r = sstv_anim_parse(&anim_info, rx_anim_byte, NULL);
        if(r != 0) rxlog("A header %d: %dx%d n=%d fps=%d order=%d len=%d", r, anim_info.w, anim_info.h, anim_info.n, anim_info.fps, anim_info.order_len, rx.note_len);
        if(r < 0) anim_hdr_len = -1;
        if(r > 0) {
            anim_frames = pd->system->realloc(anim_frames, (size_t)anim_info.n * (size_t)sstv_anim_frame_bytes(&anim_info));
            sstv_anim_dec_init(&anim_dec, &anim_info, r, rx.note_len, anim_frames);
            anim_rowpos = pd->system->realloc(anim_rowpos, sizeof(uint16_t) * (size_t)anim_info.n * (size_t)anim_info.h);
            memset(anim_rowpos, 0xFF, sizeof(uint16_t) * (size_t)anim_info.n * (size_t)anim_info.h);
            anim_dec.row_pos = anim_rowpos;
            anim_hdr_len = r;
            if(anim_bm) pd->graphics->freeBitmap(anim_bm);
            anim_bm = pd->graphics->newBitmap(anim_info.w, anim_info.h, kColorWhite);
        }
    }
    /* segment CRCs, rechecked whenever frames arrive or get repaired: a segment whose bytes come
     * right after a mismatch (a resend overwrote a false-accepted frame) is decoded again */
    int stamp = rx.frames_ok + rx.frames_repaired;
    if(anim_hdr_len > 0 && (stamp != anim_rx_stamp || final)) {
        anim_rx_stamp = stamp;
        for(int sg = 0; sg < sstv_anim_segs(anim_info.n); sg++) {
            if(anim_seg_ok[sg] == 1) continue;
            int r = sstv_anim_seg_check(&anim_dec, sg, rx_anim_byte, NULL);
            if(r == 1) {
                bool redo = anim_seg_ok[sg] == -1;
                for(int f = sg * SSTV_ANIM_SEG; f < (sg + 1) * SSTV_ANIM_SEG && f < anim_info.n; f++)
                    if(anim_dec.state[f] == 3) redo = true;
                if(redo) sstv_anim_dec_rewind(&anim_dec, sg);
                rxlog("A seg %d CRC ok%s", sg, redo ? ", decoding again" : "");
            } else if(r == -1 && anim_seg_ok[sg] != -1) {
                rxlog("A seg %d CRC MISMATCH", sg);
            }
            anim_seg_ok[sg] = (int8_t)r;
        }
    }
    if(anim_hdr_len > 0 && !sstv_anim_dec_done(&anim_dec)) {
        int fd0 = anim_dec.frames_done, seg0 = anim_dec.seg;
        bool stuck;
        sstv_anim_dec_run(&anim_dec, rx_anim_byte, NULL, budget, final, &stuck);
        if(anim_dec.frames_done != fd0 || anim_dec.seg != seg0 || final)
            rxlog("A frames_done=%d/%d seg=%d frame=%d pix=%d pos=%d end=%d stuck=%d final=%d corrupt=%d", anim_dec.frames_done, anim_info.n, anim_dec.seg,
                  anim_dec.frame, anim_dec.pix, anim_dec.pos, anim_dec.end, stuck, final, anim_dec.corrupt);
    }
}

/* Base file name for the current pass (no extension). */
static void base_name(char* out, size_t cap, const char* fallback_prefix, const char* ext) {
    if(name_is_blank(current_name)) {
        next_named(fallback_prefix, ext, out, cap);
        return;
    }
    char t[64];
    sanitize_name(t, sizeof t, current_name);
    char* dot = strrchr(t, '.');
    if(dot && strcmp(dot, ".pbm") == 0) *dot = 0;
    strncpy(out, t, cap - 1);
    out[cap - 1] = 0;
}

static void file_write(void* ctx, const void* data, int len) {
    pd->file->write((SDFile*)ctx, data, (unsigned)len);
}

/* .sfa: "SFA1" w h n fps order_len order[] frames[] (XBM) - what the gallery plays. */
static bool write_sfa(const char* fname, const sstv_anim_info* a, const uint8_t* frames) {
    SDFile* f = pd->file->open(fname, kFileWrite);
    if(!f) return false;
    uint8_t h[9] = {'S', 'F', 'A', '1', (uint8_t)a->w, (uint8_t)a->h, (uint8_t)a->n, (uint8_t)a->fps, (uint8_t)a->order_len};
    pd->file->write(f, h, 9);
    pd->file->write(f, a->order, (unsigned)a->order_len);
    pd->file->write(f, frames, (unsigned)(a->n * sstv_anim_frame_bytes(a)));
    pd->file->close(f);
    return true;
}

static bool read_sfa(const char* fname, sstv_anim_info* a, uint8_t** frames) {
    SDFile* f = pd->file->open(fname, kFileReadData);
    if(!f) return false;
    uint8_t h[9];
    bool ok = pd->file->read(f, h, 9) == 9 && memcmp(h, "SFA1", 4) == 0 && h[4] && h[5] && h[6] && h[8];
    if(ok) {
        memset(a, 0, sizeof *a);
        a->w = h[4];
        a->h = h[5];
        a->n = h[6];
        a->fps = h[7] ? h[7] : 2;
        a->order_len = h[8];
        int fb = sstv_anim_frame_bytes(a);
        *frames = pd->system->realloc(*frames, (size_t)(a->n * fb));
        ok = pd->file->read(f, a->order, (unsigned)a->order_len) == a->order_len &&
             pd->file->read(f, *frames, (unsigned)(a->n * fb)) == a->n * fb;
        for(int i = 0; ok && i < a->order_len; i++)
            if(a->order[i] >= a->n) ok = false;
    }
    pd->file->close(f);
    return ok;
}

/* Frames that decoded completely in a segment whose CRC matched. */
static int anim_frames_ok(void) {
    if(anim_hdr_len <= 0) return 0;
    int n = 0;
    for(int f = 0; f < anim_info.n; f++) n += anim_dec.state[f] == 2 && anim_seg_ok[f / SSTV_ANIM_SEG] == 1;
    return n;
}

/* Saves the received animation: .sfa for the gallery and an animated .gif (2x) for a computer.
 * Undecodable frames (a hole the outer code could not fill) stay as far as they got. */
static void save_anim(void) {
    if(!anim_dirty) return;
    anim_pump(1 << 30, true);
    if(anim_hdr_len <= 0) return;
    /* a segment whose CRC never matched decoded from a wrong byte: its frames hold the last
     * good frame instead of garbage */
    int fb = sstv_anim_frame_bytes(&anim_info);
    for(int f = 0; f < anim_info.n; f++) {
        if(anim_seg_ok[f / SSTV_ANIM_SEG] != -1) continue;
        if(f > 0) memcpy(anim_frames + f * fb, anim_frames + (f - 1) * fb, (size_t)fb);
        else memset(anim_frames, 0, (size_t)fb);
    }
    rxlog("A saved: %d/%d frames verified", anim_frames_ok(), anim_info.n);
    if(anim_frames_ok() == anim_info.n) sstv_rx_forget(&rx); /* complete: a resend starts over */
    char base[NAME_CAP], fname[NAME_CAP + 8];
    base_name(base, sizeof base, "anim_", "sfa");
    snprintf(fname, sizeof fname, "%s.sfa", base);
    write_sfa(fname, &anim_info, anim_frames);
    snprintf(fname, sizeof fname, "%s.gif", base);
    SDFile* f = pd->file->open(fname, kFileWrite);
    if(f) {
        int delay = anim_info.fps > 0 ? 100 / anim_info.fps : 50;
        export_gif(anim_info.w, anim_info.h, 2, anim_frames, sstv_anim_frame_bytes(&anim_info), anim_info.order,
                   anim_info.order_len, delay, file_write, f);
        pd->file->close(f);
    }
    anim_dirty = false;
}

/* One XBM frame into anim_bm-sized bitmap `bm` (1 = black in XBM, 0 = black on the Playdate). */
static void xbm_to_bitmap(const uint8_t* f, int w, int h, LCDBitmap* bm) {
    int bw, bh, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(bm, &bw, &bh, &rb, NULL, &data);
    int stride = sstv_anim_stride(w);
    for(int y = 0; y < h && y < bh; y++) {
        uint8_t* d = data + y * rb;
        memset(d, 0xFF, (size_t)rb);
        for(int x = 0; x < w; x++)
            if((f[y * stride + (x >> 3)] >> (x & 7)) & 1) d[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
    }
}

/* Largest whole scale that fits w x h into the box. */
static int fit_scale(int w, int h, int bw, int bh) {
    int s = bw / w < bh / h ? bw / w : bh / h;
    return s < 1 ? 1 : s;
}

/* Persists whatever the current reception produced (a picture, a note or an animation). */
static void save_received(void) {
    if(rx.anim) save_anim();
    else if(rx.note) save_note();
    else save_image();
}

/* ---- gallery data ---- */

/* System-menu "filter" for the gallery: 0 all, 1 images, 2 animations, 3 notes. The main
 * menu (tile, "N saved") always counts everything. */
static int gallery_filter;
static const uint8_t filter_kind[4] = {0xff, 0, 2, 1}; /* gallery_kind each filter keeps */

static void gallery_list_cb(const char* path, void* userdata) {
    int filter = *(const int*)userdata;
    size_t n = strlen(path);
    if(n < 5 || gallery_count >= MAX_GALLERY) return;
    bool pbm = strcmp(path + n - 4, ".pbm") == 0;
    bool txt = strcmp(path + n - 4, ".txt") == 0;
    bool sfa = strcmp(path + n - 4, ".sfa") == 0;
    if(!pbm && !txt && !sfa) return;
    if(strncmp(path, "rxlog", 5) == 0) return; /* the reception log, not a note */
    uint8_t kind = txt ? 1 : sfa ? 2 : 0;
    if(filter && kind != filter_kind[filter]) return;
    strncpy(gallery_names[gallery_count], path, NAME_CAP - 1);
    gallery_names[gallery_count][NAME_CAP - 1] = 0;
    gallery_kind[gallery_count] = kind;
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

/* Shrinks a 1-bit picture into the white bitmap `t` at (ox, oy), dw x dh: each target pixel
 * takes the share of black in its source box, dithered back to 1 bit (Floyd-Steinberg).
 * Sampling one source pixel per target pixel (what thumbnails did before) picked a single dot
 * of the picture's own dither pattern, so halftones and gradients came out as noise or flat
 * fields. msb: PBM rows (bit 7 = leftmost); otherwise XBM (bit 0 = leftmost). 1 = black. */
static void dither_down(const uint8_t* src, int sw, int sh, int stride, bool msb, LCDBitmap* t, int ox, int oy, int dw, int dh) {
    int bw, bh, rb;
    uint8_t* data;
    pd->graphics->getBitmapData(t, &bw, &bh, &rb, NULL, &data);
    int16_t* err = pd->system->realloc(NULL, sizeof(int16_t) * 2 * (size_t)(dw + 2));
    if(!err) return;
    memset(err, 0, sizeof(int16_t) * 2 * (size_t)(dw + 2));
    int16_t* cur = err + 1; /* [-1 .. dw] */
    int16_t* nxt = err + dw + 3;
    for(int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
        if(y1 <= y0) y1 = y0 + 1;
        for(int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
            if(x1 <= x0) x1 = x0 + 1;
            int black = 0;
            for(int sy = y0; sy < y1; sy++) {
                const uint8_t* row = src + sy * stride;
                for(int sx = x0; sx < x1; sx++) black += msb ? (row[sx >> 3] >> (7 - (sx & 7))) & 1 : (row[sx >> 3] >> (sx & 7)) & 1;
            }
            int v = black * 255 / ((y1 - y0) * (x1 - x0)) + cur[x]; /* darkness 0..255 */
            int out = v >= 128 ? 255 : 0;
            int e = v - out;
            cur[x + 1] += (int16_t)(e * 7 / 16);
            nxt[x - 1] += (int16_t)(e * 3 / 16);
            nxt[x] += (int16_t)(e * 5 / 16);
            nxt[x + 1] += (int16_t)(e / 16);
            int px = ox + x, py = oy + y;
            if(out && px >= 0 && py >= 0 && px < bw && py < bh) data[py * rb + (px >> 3)] &= (uint8_t)~(0x80 >> (px & 7));
        }
        int16_t* tmp = cur;
        cur = nxt;
        nxt = tmp;
        memset(nxt - 1, 0, sizeof(int16_t) * (size_t)(dw + 2));
    }
    pd->system->realloc(err, 0);
}

/* 400x240 -> 100x60 thumbnail with rounded corners. */
static LCDBitmap* make_thumb(const uint8_t* px) {
    LCDBitmap* t = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorWhite);
    dither_down(px, SSTV_WIDTH, SSTV_HEIGHT, SSTV_LINE_BYTES, true, t, 0, 0, THUMB_W, THUMB_H);
    if(thumb_mask) pd->graphics->setBitmapMask(t, thumb_mask);
    return t;
}

/* Renders a note as a mini sticky-note thumbnail (same template, scaled down). */
static LCDBitmap* make_note_thumb(const char* text) {
    LCDBitmap* t = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorWhite);
    pd->graphics->pushContext(t);
    draw_sticky_note(pd, font_notes, 0, 0, THUMB_W, THUMB_H, text);
    pd->graphics->popContext();
    if(thumb_mask) pd->graphics->setBitmapMask(t, thumb_mask);
    return t;
}

/* An animation's thumbnail: its first frame, scaled to fit, on white. */
static LCDBitmap* make_anim_thumb(const char* name) {
    sstv_anim_info a;
    uint8_t* fr = NULL;
    LCDBitmap* t = NULL;
    if(read_sfa(name, &a, &fr)) {
        t = pd->graphics->newBitmap(THUMB_W, THUMB_H, kColorWhite);
        /* fit, keeping the aspect: 128x64 -> 100x50 */
        int dw = THUMB_W, dh = a.h * THUMB_W / a.w;
        if(dh > THUMB_H) { dh = THUMB_H; dw = a.w * THUMB_H / a.h; }
        dither_down(fr + a.order[0] * sstv_anim_frame_bytes(&a), a.w, a.h, sstv_anim_stride(a.w), false, t, (THUMB_W - dw) / 2,
                    (THUMB_H - dh) / 2, dw, dh);
        if(thumb_mask) pd->graphics->setBitmapMask(t, thumb_mask);
    }
    if(fr) pd->system->realloc(fr, 0);
    return t;
}

static void free_thumbs(void) {
    for(int i = 0; i < MAX_GALLERY; i++) {
        if(thumbs[i]) pd->graphics->freeBitmap(thumbs[i]);
        thumbs[i] = NULL;
    }
}

static void gallery_ensure_thumbs(int first, int count);
static PDMenuItem* filter_item;
static unsigned long long mtime_key(const FileStat* s) {
    return (unsigned long long)s->m_year * 10000000000ULL + (unsigned long long)s->m_month * 100000000ULL +
           (unsigned long long)s->m_day * 1000000ULL + (unsigned long long)s->m_hour * 10000ULL +
           (unsigned long long)s->m_minute * 100ULL + (unsigned long long)s->m_second;
}

/* Lists the saved items of kind `filter` (see gallery_filter), newest saved first. */
static void gallery_scan(int filter) {
    save_received();
    free_thumbs();
    gallery_count = 0;
    pd->file->listfiles(".", gallery_list_cb, &filter, 0);
    if(gallery_count > 1) {
        int idx[MAX_GALLERY];
        unsigned long long key[MAX_GALLERY];
        for(int i = 0; i < gallery_count; i++) {
            idx[i] = i;
            FileStat st;
            key[i] = pd->file->stat(gallery_names[i], &st) == 0 ? mtime_key(&st) : 0ULL;
        }
        for(int i = 1; i < gallery_count; i++) { /* insertion sort, newest first */
            int j = i;
            while(j > 0 && key[idx[j - 1]] < key[idx[j]]) {
                int t = idx[j - 1];
                idx[j - 1] = idx[j];
                idx[j] = t;
                j--;
            }
        }
        static char names2[MAX_GALLERY][NAME_CAP];
        static uint8_t kind2[MAX_GALLERY];
        for(int i = 0; i < gallery_count; i++) {
            strncpy(names2[i], gallery_names[idx[i]], NAME_CAP - 1);
            names2[i][NAME_CAP - 1] = 0;
            kind2[i] = gallery_kind[idx[i]];
        }
        memcpy(gallery_names, names2, sizeof gallery_names);
        memcpy(gallery_kind, kind2, sizeof gallery_kind);
    }
}

/* system menu: gallery filter; in the gallery the list refreshes at once (back to the grid) */
static void filter_menu_cb(void* ud) {
    (void)ud;
    if(!filter_item) return;
    gallery_filter = pd->system->getMenuItemValue(filter_item);
    if(ui_mode == ModeGrid || ui_mode == ModeFull || (ui_mode == ModeAbout && (about_back == ModeGrid || about_back == ModeFull))) {
        gallery_scan(gallery_filter);
        gallery_index = 0;
        gallery_page = 0;
        gallery_ensure_thumbs(0, GRID_PER_PAGE);
        if(ui_mode == ModeAbout) about_back = ModeGrid;
        else ui_mode = ModeGrid;
    }
}

static bool gallery_load_full(int idx) {
    if(idx < 0 || idx >= gallery_count) return false;
    full_is_anim = false;
    if(gallery_kind[idx] == 2) {
        full_is_note = false;
        full_is_anim = read_sfa(gallery_names[idx], &full_anim, &full_anim_frames);
        anim_play_pos = 0;
        anim_play_ms = pd->system->getCurrentTimeMilliseconds();
        return full_is_anim;
    }
    if(gallery_kind[idx] == 1) {
        full_is_note = true;
        full_scroll = 0;
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
        if(gallery_kind[i] == 2) {
            thumbs[i] = make_anim_thumb(gallery_names[i]);
        } else if(gallery_kind[i] == 1) {
            if(read_note(gallery_names[i], thumb_txt, sizeof thumb_txt)) thumbs[i] = make_note_thumb(thumb_txt);
        } else if(read_pbm(gallery_names[i], px)) {
            thumbs[i] = make_thumb(px);
        }
    }
    pd->system->realloc(px, 0);
}

static void gallery_open(void) {
    stop_mic();
    gallery_scan(gallery_filter);
    ui_mode = ModeGrid;
    gallery_index = 0;
    gallery_page = 0;
    gallery_ensure_thumbs(0, GRID_PER_PAGE);
}

static void menu_open(void) {
    gallery_scan(0);
    if(menu_thumb) {
        pd->graphics->freeBitmap(menu_thumb);
        menu_thumb = NULL;
    }
    if(gallery_count > 0) {
        int last = 0; /* the list is newest first */
        if(gallery_kind[last] == 2) {
            menu_thumb = make_anim_thumb(gallery_names[last]);
        } else if(gallery_kind[last] == 1) {
            if(read_note(gallery_names[last], thumb_txt, sizeof thumb_txt)) menu_thumb = make_note_thumb(thumb_txt);
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
    /* full-size placeholder (checker of the preview) drawn below the drawing line */
    if(!ph_bm) ph_bm = pd->graphics->newBitmap(SSTV_WIDTH, SSTV_HEIGHT, kColorWhite);
    int pw, ph2, prb;
    uint8_t* pdw;
    pd->graphics->getBitmapData(ph_bm, &pw, &ph2, &prb, NULL, &pdw);
    for(int yy = 0; yy < SSTV_HEIGHT; yy++) {
        for(int xx = 0; xx < SSTV_WIDTH; xx++) {
            int ink = placeholder_bit(xx, yy) && ((xx + yy) & 1) == 0;
            if(ink) pdw[yy * prb + (xx >> 3)] &= (uint8_t)~(0x80 >> (xx & 7));
            else pdw[yy * prb + (xx >> 3)] |= (uint8_t)(0x80 >> (xx & 7));
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
    anim_reset();
    memset(note_buf, 0, sizeof note_buf); /* rx.note itself is owned by the START/END flag */
    note_scroll = 0;
    note_follow = true;
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
    revealed = 0.0f;
    rx_paused = false; /* a fresh session clears any leftover pause banner */
    sheet_y = LCD_ROWS;
    bar_y = 0;
    image_dirty = false;
    finished = false;
    started = false;
    total_frames = 0; /* unknown until a START announces it */
    sstv_demod_reset_counters(&demod);
    sstv_rx_reset_counters(&rx);
}

static void finish(unsigned now) {
    rxlog("FINISH in=%d ok=%d bad=%d rs=%d repaired=%d groups_repaired=%d repairs_failed=%d fly=%d dropped=%u segs=%d anim=%d",
          rx.frames_in, rx.frames_ok, rx.frames_bad, rx.rs_corrected, rx.frames_repaired, rx.groups_repaired, rx.repairs_failed,
          demod.flywheel_frames, (unsigned)mic_dropped, segs_done, rx.anim);
    finished = true;
    started = false;
    finished_ms = now;
    finished_lines = segs_done / 2;
    lines_reached = SSTV_HEIGHT; /* pull the whole sheet in, gaps and all */
    revealed = SSTV_HEIGHT;
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
    int phys = SSTV_ILEAVE + (int)((float)(now - started_ms) * 1000.0f / (float)PHYS_US_NOW);
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
    rxlog("%s name=\"%s\" total=%d note=%d anim=%d coded=%d len=%d data_frames=%d merged=%d", is_end ? "END" : "START", name, frames, rx.note,
          rx.anim, coded, rx.note_len, rx.data_frames, rx.merged);
    if(is_end) {
        log_missing("END");
        finish(now); /* the transmitter is done: save and announce */
        chime_ok = rx.anim ? (anim_hdr_len > 0 && anim_frames_ok() == anim_info.n)
                 : rx.note ? (rx.note_len > 0 && (int)strlen(note_buf) >= rx.note_len) : segs_done == SSTV_NUM_SEGS;
        chime_at = now + CHIME_DELAY_MS;
        if(chime_at == 0) chime_at = 1;
        return;
    }
    calib_active = false; /* a real pass ends calibration */
    calib_streak = 0;
    /* A new transmission starts: keep what the previous one produced, then start fresh. */
    save_received();
    clear_image();
    started = true;
    started_ms = now;
    total_frames = frames;
    if(name_is_blank(name)) {
        if(rx.anim) next_named("anim_", "sfa", current_name, NAME_CAP);
        else if(rx.note) next_note_name(current_name, NAME_CAP); /* notes are numbered note_N (always Latin) */
        else next_untitled(current_name, NAME_CAP);
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
            started_ms = now - (unsigned)((uint64_t)(n - SSTV_ILEAVE) * PHYS_US_NOW / 1000);
            last_data_ms = now;
        }
    }
}

static void on_note(void* ctx, int chunk, int nbytes, const uint8_t* text) {
    (void)ctx;
    if(rx.anim) { /* the bytes stay in rx.data; anim_pump reads them from there */
        anim_dirty = true;
        last_data_ms = pd->system->getCurrentTimeMilliseconds();
        return;
    }
    int off = chunk * SSTV_NOTE_CHUNK;
    for(int k = 0; k < nbytes && off + k < SSTV_NOTE_MAX_BYTES; k++) note_buf[off + k] = (char)text[k];
    note_dirty = true;
}

/* ---- calibration: the Flipper loops lock frames (menu > Calibrate); every frame is compared
 * with the known pattern, so the bit error rate is exact. The screen opens on its own after a
 * run of lock frames longer than a preamble's, and closes 3 s after they stop. ---- */
#define CALIB_HIST 60 /* frames on the graph, ~21 s */
#define CALIB_STREAK 4 /* consecutive lock frames to open (a preamble has SSTV_LOCK_FRAMES = 3) */
/* Quality = the chance that a frame with this bit error rate passes the inner RS code: a byte is
 * bad with p = 1 - (1 - ber)^8, the frame decodes blind with at most SSTV_PARITY_BYTES / 2 bad
 * bytes of SSTV_FRAME_BYTES (erasures help a little more). The old linear scale (100 at 0 %,
 * 0 at 10 % bit errors) read 85 % where frames were already failing. */
static float frame_ok_prob(float ber) {
    float pb = 1.0f - powf(1.0f - ber, 8.0f);
    if(pb >= 1.0f) return 0.0f;
    double term = pow(1.0 - (double)pb, SSTV_FRAME_BYTES), sum = 0.0; /* P(0 bad bytes) */
    for(int k = 0; k <= SSTV_PARITY_BYTES / 2; k++) {
        sum += term;
        term *= (double)(SSTV_FRAME_BYTES - k) / (double)(k + 1) * (double)pb / (1.0 - (double)pb);
    }
    return (float)(sum > 1.0 ? 1.0 : sum);
}
/* A frame counts towards opening the screen when it differs from the lock frame in at most this
 * many of 360 bits: looser than SSTV_LOCK_MATCH_BITS (60) so a weak spot still opens it; a random
 * frame differs in ~164 +- 9 bits. */
#define CALIB_OPEN_BITS 120
static bool calib_active;
static unsigned calib_last_ms;
static int calib_streak, calib_n, calib_fly_prev;
static uint8_t calib_score[CALIB_HIST], calib_sync[CALIB_HIST];
static float calib_ber;

static void calib_frame(const uint8_t* frame, unsigned now) {
    int dist = sstv_lock_distance(frame);
    bool real = demod.flywheel_frames == calib_fly_prev;
    calib_fly_prev = demod.flywheel_frames;
    if(dist <= CALIB_OPEN_BITS) {
        if(calib_streak == 0 && (!started || finished) && !calib_active && now - start_chime_ms > 8000) play_start_chime(now);
        calib_last_ms = now;
        if(++calib_streak >= CALIB_STREAK && !calib_active && (!started || finished)) {
            calib_active = true;
            calib_n = 0;
        }
    } else {
        calib_streak = 0;
    }
    if(!calib_active) return;
    /* Once open, any frame keeps the screen up: far away the lock frames come in with more than
     * SSTV_LOCK_MATCH_BITS errors and stop counting as lock frames, but they still score (0).
     * It closes when no frame at all arrives for 3 s, or when a real pass starts. */
    calib_last_ms = now;
    float ber = (float)dist / (float)SSTV_FRAME_BITS;
    calib_ber = ber;
    float sc = 100.0f * frame_ok_prob(ber);
    int i = calib_n % CALIB_HIST;
    calib_score[i] = (uint8_t)(sc + 0.5f);
    calib_sync[i] = real;
    calib_n++;
}

static void play_start_chime(unsigned now) {
    start_chime_ms = now;
    if(!start_synth[0] || !start_synth[1]) return;
    rxlog("CHIME start");
    uint32_t t = pd->sound->getCurrentTime();
    uint32_t step = (uint32_t)(CHIME_NOTE_S * 44100.0f);
    pd->sound->synth->playNote(start_synth[0], 2093.0f, 0.8f, CHIME_NOTE_S, t); /* C7 */
    pd->sound->synth->playNote(start_synth[1], 2637.0f, 0.8f, CHIME_NOTE_S, t + step); /* E7 */
}

static void on_phys(void* ctx, const uint8_t* frame, const float* soft) {
    (void)ctx;
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    calib_frame(frame, now);
    led_frame_ms = now;
    int before = rx.blocks_ok;
    int ok0 = rx.frames_ok, bad0 = rx.frames_bad, rs0 = rx.rs_corrected, rep0 = rx.frames_repaired, fail0 = rx.repairs_failed;
    sstv_rx_phys(&rx, frame, soft);
    if(rx.blocks_ok != before) { led_block_ms = led_frame_ms; last_block_type = rx.last_type; last_block_ms = led_frame_ms; }
    /* frame quality: mean |soft| and bits under 0.2 (weak decisions) */
    float sum = 0.0f;
    int weak = 0;
    for(int i = 0; i < SSTV_FRAME_BITS; i++) {
        float m = soft[i] < 0 ? -soft[i] : soft[i];
        sum += m;
        if(m < 0.2f) weak++;
    }
    int exp_idx = (started && total_frames > 0) ? SSTV_ILEAVE + (int)((float)(now - started_ms) * 1000.0f / (float)PHYS_US_NOW) : -1;
    int lock = sstv_lock_distance(frame);
    rxlog("F in=%d exp=%d conf=%.2f weak=%d fly=%d lock=%d", rx.frames_in, exp_idx, (double)(sum / SSTV_FRAME_BITS), weak, demod.flywheel_frames, lock);
    if(rx.frames_ok != ok0 || rx.frames_bad != bad0)
        rxlog("B type=%d ok+%d bad+%d rs+%d repaired+%d%s", rx.last_type, rx.frames_ok - ok0, rx.frames_bad - bad0, rx.rs_corrected - rs0,
              rx.frames_repaired - rep0, rx.repairs_failed != fail0 ? " REPAIR-FAILED" : "");
    else if(rx.frames_repaired != rep0 || rx.repairs_failed != fail0)
        rxlog("R repaired+%d%s", rx.frames_repaired - rep0, rx.repairs_failed != fail0 ? " REPAIR-FAILED" : "");
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

/* Asked when Receive opens, not at launch: the menu and the gallery need no microphone. The
 * system remembers the answer, so after the first time this returns at once without a dialog
 * (and picks up a change made in Settings). */
static void ask_mic(void) {
    enum accessReply r = pd->sound->requestMicAccess(
        /* the system shows "<name> would like to use the microphone: <this>" with room for about
         * three lines, and its font has no comma */
        "to receive incoming transmissions.",
        mic_access_cb, NULL);
    if(r == kAccessAllow) mic_state = 1;
    else if(r == kAccessDeny) mic_state = -1;
    else mic_state = 2; /* kAccessAsk: the dialog is up, mic_access_cb follows */
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
    strncpy(rec_saved, path, sizeof rec_saved - 1);
    rec_saved_ms = pd->system->getCurrentTimeMilliseconds();
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
static void draw_title_bar_font(const char* title, const char* hint, LCDFont* title_font) {
    pd->graphics->fillRect(0, 0, LCD_COLUMNS, BAR_H, kColorBlack);
    use_font(title_font);
    int ty = (BAR_H - font_height()) / 2;
    draw_text_white(title, 10, ty);
    if(hint) {
        use_font(font_glyph); /* hints use the button-glyph font */
        draw_text_white(hint, LCD_COLUMNS - 10 - text_width(hint), ty);
    }
}
static void draw_title_bar(const char* title, const char* hint) {
    draw_title_bar_font(title, hint, font_glyph);
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

/* System-menu screens:
 * About: name, version and build on the left, authorship at the bottom, the QR code to GitHub
 *   on the right under its caption (the same card as the Flipper's About);
 * Support: the Buy Me a Coffee button and the QR code to the page. */
static void draw_about_support(void) {
    const int qr = 145, qx = LCD_COLUMNS - 20 - 150 + (150 - qr) / 2, qy = 66;
    use_font(font_big);
    draw_text("Like it?", 20, 52);
    use_font(font_ui);
    draw_text("Support me making", 20, 92);
    draw_text("fun little things.", 20, 110);
    /* their button (a picture: the Playdate has no browser), level with the bottom of the code */
    if(bmc_button) {
        int bw, bh;
        pd->graphics->getBitmapData(bmc_button, &bw, &bh, NULL, NULL, NULL);
        pd->graphics->drawBitmap(bmc_button, 20, qy + qr - bh, kBitmapUnflipped);
    }
    draw_text_centered("buymeacoffee.com", qx + qr / 2, qy - font_height() - 6);
    if(qr_coffee) pd->graphics->drawBitmap(qr_coffee, qx, qy, kBitmapUnflipped);
}

static void draw_about_app(void) {
    const int qr = 150, qx = LCD_COLUMNS - 20 - qr, qy = 66;
    use_font(font_big);
    draw_text("SFTV RX", 20, 52);
    use_font(font_ui);
    char t[32];
    snprintf(t, sizeof t, "version %s", SFTV_VERSION);
    draw_text(t, 20, 92);
    snprintf(t, sizeof t, "build %d", SFTV_BUILD);
    draw_text(t, 20, 110);
    draw_text("© 2026 negenii", 20, qy + qr - font_height() + 2); /* bottom line level with the code */
    draw_text_centered("github.com", qx + qr / 2, qy - font_height() - 6);
    if(qr_github) pd->graphics->drawBitmap(qr_github, qx, qy, kBitmapUnflipped);
}

static void draw_about(void) {
    pd->graphics->clear(kColorWhite);
    draw_title_bar(about_page ? "Support" : "About", "Ⓑ back");
    if(about_page) draw_about_support();
    else draw_about_app();
}

/* system menu: About (ud = 0) and Support (ud = 1) share one screen mode */
static void about_menu_cb(void* ud) {
    if(ui_mode != ModeAbout) about_back = ui_mode;
    about_page = ud ? 1 : 0;
    ui_mode = ModeAbout;
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
    draw_console(cx, cy, (started && !rx.note) ? preview_bitmap : NULL);
    use_font(font_ui);
    if(started) {
        draw_spout(dx + 70, dy + 2, cx + CONSOLE_SCREEN_X + CONSOLE_SCREEN_W / 2, cy + CONSOLE_SCREEN_Y + CONSOLE_SCREEN_H / 2, (float)(now % 1200) / 1200.0f);
        if(rx.note) {
            draw_text_centered(rx.anim ? (anim_hdr_len < 0 ? "Animation header unreadable" : "Receiving animation") : "Receiving note", text_cx, 200);
        } else if(segs_done == 0) {
            /* Preview + intro repair progress: the bar spans everything before the picture
             * (tiny + preview, the schedule for coded passes, and the intro group's parity), so
             * it reaches 100% right before the picture starts. It advances by elapsed time, and
             * while the preview chunks are still arriving it is clamped between the decoded count
             * and one 4-frame block ahead, so a lost block holds it; the parity tail runs on time. */
            int intro_total = SSTV_INTRO_FRAMES + (rx.coded ? SSTV_SCHEDULE_CHUNKS : 0) + SSTV_OUTER_PARITY;
            float est = (float)(now - started_ms) / (float)(PHYS_US_NOW / 1000);
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
        const char* hint = mic_state < 0 ? "allow the microphone in Settings > Permissions" : mic_state != 1 ? "waiting for microphone permission" : "waiting for a transmission";
        draw_text_centered(hint, text_cx, 208);
    }
}

static void draw_panel(void) {
    sstv_demod_status_t st;
    sstv_demod_status(&demod, &st);
    int x0 = LCD_COLUMNS - PANEL_W;
    pd->graphics->fillRect(x0, 0, PANEL_W, LCD_ROWS, kColorWhite);
    pd->graphics->drawLine(x0, 0, x0, LCD_ROWS, 1, kColorBlack);
    int x = x0 + 8, y = 6, right = x0 + PANEL_W - 8;
    char t[64], nm[NAME_CAP];
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    bool quiet = segs_done > 0 && !finished && !rx_paused && now - last_data_ms > LOST_TIMEOUT_MS;
    use_font(font_ui);
    int lh = font_height() + 1; /* compact rows: everything stays above the hint line */

    display_name(nm, sizeof nm, current_name);
    use_font(font_ui);
    fit_text(t, sizeof t, nm, PANEL_W - 16 - 20);
    draw_text(t, x, y);
    y += lh + 3;
    /* state and mode pills */
    const char* state = rx_paused ? "PAUSE" : finished ? "DONE" : segs_done == SSTV_NUM_SEGS ? "DONE" : quiet ? "LOST" : segs_done > 0 || started ? "RX" : "IDLE";
    int px = x;
    px += draw_pill(state, px, y, true) + 5;
    px += draw_pill(rx.anim ? "ANI" : rx.note ? "TXT" : rx.coded ? "ZIP" : "RAW", px, y, false) + 5;
    if(rec_len >= 0) draw_pill(rec_prefix[0] == 't' ? "TEST" : "REC", px, y, true);
    y += font_height() + 9;
    /* progress: lines for a picture, characters for a note */
    int bar_w = PANEL_W - 16;
    pd->graphics->drawRoundRect(x, y, bar_w, 8, 4, 1, kColorBlack);
    int got, total_units;
    if(rx.anim) {
        got = anim_hdr_len > 0 ? anim_dec.frames_done : 0;
        total_units = anim_hdr_len > 0 ? anim_info.n : 1;
    } else if(rx.note) {
        got = (int)strlen(note_buf);
        total_units = rx.note_len > 0 ? rx.note_len : (got > 0 ? got : 1);
    } else {
        got = segs_done;
        total_units = SSTV_NUM_SEGS;
    }
    int fill = (bar_w - 2) * got / total_units;
    if(fill > bar_w - 2) fill = bar_w - 2;
    if(fill > 0) pd->graphics->fillRoundRect(x + 1, y + 1, fill, 6, 3, kColorBlack);
    y += 11;
    if(rx.anim) snprintf(t, sizeof t, "%d / %d frames", got, anim_hdr_len > 0 ? anim_info.n : 0);
    else if(rx.note) snprintf(t, sizeof t, "%d / %d chars", (int)strlen(note_buf), rx.note_len);
    else snprintf(t, sizeof t, "%d / %d lines", segs_done / 2, SSTV_HEIGHT);
    draw_text(t, x, y);
    y += lh + 4;
    /* counters: label left, value right; ok/bad and rs/repaired combined into one line each */
    char v0[16], v1[16], v2[16], v3[16], v4[16], v5[16], v6[16];
    snprintf(v6, sizeof v6, "%u/%d", (unsigned)mic_dropped, dsp_load);
    snprintf(v0, sizeof v0, "%d", rx.frames_in);
    snprintf(v1, sizeof v1, "%d", preview_chunks);
    snprintf(v2, sizeof v2, "%d/%d", rx.frames_ok, rx.frames_bad);
    snprintf(v3, sizeof v3, "%d/%d", rx.rs_corrected, rx.frames_repaired);
    snprintf(v4, sizeof v4, "%d", demod.flywheel_frames);
    snprintf(v5, sizeof v5, "%d", (int)mic_dropped);
    struct { const char* k; const char* v; } rows[] = {
        {"recv", v0}, {"preview", v1}, {"frames ok/bad", v2}, {"rs fixed/fixed", v3},
        {"flywheel", v4}, {"dropped / dsp %", v6},
    };
    for(size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        use_font(font_ui);
        draw_text(rows[i].k, x, y);
        use_font(font_num);
        draw_text(rows[i].v, right - text_width(rows[i].v), y + 1);
        y += lh;
    }
    /* tone: one lamp per data tone (8 in the HF band, 17.0-20.5 kHz), the one being heard
     * filled (the sync is a pattern of these tones since revision 13) */
    y += 4;
    use_font(font_ui);
    draw_text("tone", x, y);
    {
        int r = 5, step = 2 * r + 4, cx0 = x + 40, ccy = y + font_height() / 2;
        int nt = demod.num_tones - demod.first_det; /* lamps: data tones (the audible band's silence has none) */
        if(nt > SSTV_MAX_TONES) nt = SSTV_MAX_TONES;
        for(int k = 0; k < nt; k++) {
            int ccx = cx0 + k * step;
            if(st.tone == k + demod.first_det) pd->graphics->fillEllipse(ccx, ccy - r, 2 * r, 2 * r, 0.0f, 0.0f, kColorBlack);
            else pd->graphics->drawEllipse(ccx, ccy - r, 2 * r, 2 * r, 1, 0.0f, 0.0f, kColorBlack);
        }
    }
    /* hints */
    use_font(font_glyph); /* 22 px line (the button glyphs): sized from the font, inside the screen */
    int hy = LCD_ROWS - font_height() - 1;
    pd->graphics->drawLine(x0, hy - 3, LCD_COLUMNS, hy - 3, 1, kColorBlack);
    draw_text("Ⓐ clear   Ⓑ menu", x, hy);
}

/* Banner over the picture for a few seconds after the END frame. */
static void draw_finished_banner(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(!finished || now - finished_ms > 6000) return;
    char t[80], nm[NAME_CAP];
    display_name(nm, sizeof nm, current_name);
    use_font(font_ui);
    if(rx.anim)
        snprintf(t, sizeof t, "Received \"%s\"   %d/%d frames", nm, anim_frames_ok(), anim_hdr_len > 0 ? anim_info.n : 0);
    else if(rx.note)
        snprintf(t, sizeof t, "Received note \"%s\"  %d chars", nm, rx.note_len);
    else
        snprintf(t, sizeof t, "Received \"%s\"   %d/%d lines", nm, finished_lines, SSTV_HEIGHT);
    int w = text_width(t) + 24, h = font_height() + 16;
    int x = (LCD_COLUMNS - w) / 2, y = LCD_ROWS - h - 14;
    pd->graphics->fillRoundRect(x, y, w, h, h / 2, kColorBlack);
    draw_text_white(t, x + 12, y + 8);
}

/* Share of the pass sent so far by the announced timeline: START gives the physical frame count
 * (already after compression) and every frame lasts SSTV_PHYS_US, so the end time is known up
 * front. started_ms is the end of START (re-anchored on RESUME); `enow` is held while paused.
 * Returns -1 when the START was missed and the total is unknown. */
static float pass_progress(unsigned enow) {
    if(!started || total_frames <= 0) return -1.0f;
    float phys = (float)SSTV_ILEAVE + (float)(enow - started_ms) * 1000.0f / (float)PHYS_US_NOW;
    if(phys < 0.0f) phys = 0.0f;
    if(phys > (float)total_frames) phys = (float)total_frames;
    return phys / (float)total_frames;
}

/* White rounded progress bar on black (inside the pill). */
static void draw_mini_bar(int x, int y, int w, int h, float frac) {
    pd->graphics->drawRoundRect(x, y, w, h, h / 2, 1, kColorWhite);
    int fill = (int)((float)(w - 4) * frac + 0.5f);
    if(fill > 0) pd->graphics->fillRoundRect(x + 2, y + 2, fill, h - 4, (h - 4) / 2, kColorWhite);
}

/* Receive progress pill in the bottom-right corner: just a bar. Without a START the total is
 * unknown, so it holds a spinner instead. "Verifying" later takes the same corner. */
#define PILL_H 20
static void draw_progress_pill(unsigned now, unsigned enow) {
    float frac = pass_progress(enow);
    const int pad = 10, bw = 64, bh = 8;
    if(frac < 0.0f) {
        int sp = 12, w = pad + sp + pad;
        int bx = LCD_COLUMNS - w - 10, by = LCD_ROWS - PILL_H - 10;
        pd->graphics->fillRoundRect(bx, by, w, PILL_H, PILL_H / 2, kColorBlack);
        float a0 = (float)(now % 900) / 900.0f * 360.0f;
        pd->graphics->drawEllipse(bx + pad, by + (PILL_H - sp) / 2, sp, sp, 2, a0, a0 + 270.0f, kColorWhite);
        return;
    }
    int w = pad + bw + pad, bx = LCD_COLUMNS - w - 10, by = LCD_ROWS - PILL_H - 10;
    pd->graphics->fillRoundRect(bx, by, w, PILL_H, PILL_H / 2, kColorBlack);
    draw_mini_bar(bx + pad, by + (PILL_H - bh) / 2, bw, bh, frac);
}

/* Microphone recording status (Up = rec, Down = test take): a pill in the bottom-left corner
 * with a blinking dot and elapsed / total time while recording, then "Saved <file>" for 4 s. */
static void draw_rec_status(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    char t[40];
    if(rec_len >= 0) {
        int s = rec_len / REC_RATE;
        snprintf(t, sizeof t, "%s %d:%02d / %d:%02d", rec_prefix[0] == 't' ? "TEST" : "REC", s / 60, s % 60,
                 REC_SECONDS / 60, REC_SECONDS % 60);
    } else if(rec_saved[0] && now - rec_saved_ms < 4000) {
        snprintf(t, sizeof t, "Saved %s", rec_saved);
    } else {
        return;
    }
    use_font(font_ui);
    const int h = 20, pad = 10, dot = 8;
    bool live = rec_len >= 0;
    int w = pad + (live ? dot + 6 : 0) + text_width(t) + pad, bx = 10, by = LCD_ROWS - h - 10;
    pd->graphics->fillRoundRect(bx, by, w, h, h / 2, kColorBlack);
    int tx = bx + pad;
    if(live) {
        if((now / 500) % 2 == 0) pd->graphics->fillEllipse(tx, by + (h - dot) / 2, dot, dot, 0.0f, 0.0f, kColorWhite);
        else pd->graphics->drawEllipse(tx, by + (h - dot) / 2, dot, dot, 1, 0.0f, 0.0f, kColorWhite);
        tx += dot + 6;
    }
    draw_text_white(t, tx, by + (h - font_height()) / 2);
}

static void draw_calibration(void) {
    pd->graphics->clear(kColorWhite);
    draw_title_bar("Calibration", NULL);
    int n = calib_n < CALIB_HIST ? calib_n : CALIB_HIST;
    /* score: mean of the last 3 frames */
    int sum = 0, cnt = 0;
    for(int k = 0; k < 3 && k < n; k++) { sum += calib_score[(calib_n - 1 - k + CALIB_HIST) % CALIB_HIST]; cnt++; }
    int score = cnt ? sum / cnt : 0;
    char t[32];
    use_font(font_ui);
    draw_text("quality", 16, 40);
    use_font(font_big);
    snprintf(t, sizeof t, "%d%%", score);
    draw_text(t, 16, 56);
    use_font(font_ui);
    snprintf(t, sizeof t, "errors: %.1f%%", (double)(calib_ber * 100.0f));
    draw_text(t, 16, 96);
    /* "sync" while frames come in on their own sync tone; gone when it is lost */
    bool sync_ok = n > 0 && calib_sync[(calib_n - 1) % CALIB_HIST];
    if(sync_ok && pd->system->getCurrentTimeMilliseconds() - calib_last_ms < 1000) draw_text("sync", 16, 114);
    /* graph: one 4 px bar per frame, newest on the right; dashed line at 60 (~4 % bit errors,
     * where the error correction stops coping) */
    const int gx = 140, gy = 40, gw = CALIB_HIST * 4, gh = 100;
    pd->graphics->drawRect(gx - 1, gy - 1, gw + 2, gh + 2, kColorBlack);
    for(int x = gx; x < gx + gw; x += 6) pd->graphics->fillRect(x, gy + gh - gh * 60 / 100, 3, 1, kColorBlack);
    for(int k = 0; k < n; k++) {
        int idx = (calib_n - n + k) % CALIB_HIST;
        int h = calib_score[idx] * gh / 100;
        int x = gx + (CALIB_HIST - n + k) * 4;
        if(h > 0) pd->graphics->fillRect(x, gy + gh - h, 3, h, kColorBlack);
        if(!calib_sync[idx]) pd->graphics->fillRect(x, gy + gh + 3, 3, 2, kColorBlack); /* lost sync tick */
    }
    /* per-tone level in dB over the demodulator's noise floor, 0..40: they drop together as the
     * devices move apart (a level relative to the strongest tone would not) */
    static const char* labels[SSTV_HF_NUM_TONES] = {"17", "17.5", "18", "18.5", "19", "19.5", "20", "20.5"};
    use_font(font_num);
    const int by = 222, bmax = 60;
    for(int k = 0; k < demod.num_tones && k < SSTV_HF_NUM_TONES; k++) {
        float db = 10.0f * log10f(demod.gain[k] / demod.min_energy + 1e-12f);
        if(db < 0.0f) db = 0.0f;
        if(db > 40.0f) db = 40.0f;
        int h = (int)((float)bmax * db / 40.0f + 0.5f);
        int x = 16 + k * 48;
        pd->graphics->drawRect(x, by - bmax, 30, bmax, kColorBlack);
        if(h > 0) pd->graphics->fillRect(x, by - h, 30, h, kColorBlack);
        draw_text(labels[k], x + 15 - text_width(labels[k]) / 2, by + 3);
    }
}

/* The picture is a sheet that crawls in from the bottom edge as lines arrive (the Flipper
 * shows the same sheet leaving through its top edge). It covers the intro scene as it rises. */
static void draw_receive(void) {
    unsigned now = pd->system->getCurrentTimeMilliseconds();
    unsigned enow = rx_paused ? pause_anchor : now; /* the timeline stands still while paused */
    int y, frontier;
    if(rx.anim && anim_hdr_len > 0) {
        /* the animation at the largest whole scale (3x for 128x64): while it arrives, the frame
         * being decoded, a scan line at its decoding row; once complete, it plays */
        pd->graphics->clear(kColorWhite);
        const sstv_anim_info* a = &anim_info;
        int fb = sstv_anim_frame_bytes(a);
        int sc = fit_scale(a->w, a->h, LCD_COLUMNS, LCD_ROWS - BAR_H - 8);
        int ox = (LCD_COLUMNS - a->w * sc) / 2, oy = BAR_H + (LCD_ROWS - BAR_H - a->h * sc) / 2; /* under the bar */
        int show, row = -1;
        const uint8_t* fsrc;
        /* Bytes land in 100-byte blocks that can hold several small frames (dozens after an
         * outer-code repair), so the decoder runs ahead in bursts. The screen draws the decoded
         * frames one after another as a wipe: a line runs down the frame at a steady pace, the
         * new frame above it, the previous one still below; at the bottom the next frame starts.
         * On the frame still being decoded the line stops at its last decoded row. */
        /* The drawing line follows the air, not the decoder: bytes land in 100-byte blocks, so
         * a line tied to decoding ran a few rows at once and waited. The decoder notes the stream
         * position of every finished row (row_pos); the timeline gives the byte that was on the
         * air one interleave block ago (by then it has been decoded); the line goes to the row of
         * that byte. It moves as steadily as the bytes go out: slower over rows that cost many
         * bytes, faster over simple ones. It never passes a row that has not been decoded (a lost
         * block holds it until the repair), and eases toward its target. Rows count across
         * frames: g = frame * h + row. */
        const int H = a->h, total_rows = a->n * H;
        int decoded = anim_dec.frame * H + (anim_dec.frame_open ? anim_dec.pix / a->w : 0);
        if(decoded > total_rows) decoded = total_rows;
        float target = (float)decoded;
        anim_repairing = false;
        if(!finished && rx.data_frames > 0 && anim_rowpos) {
            int df = rx.data_frames, intro = SSTV_NOTE_INTRO(df);
            float phys = (float)(enow - started_ms) * 1000.0f / (float)PHYS_US_NOW; /* one block behind the air */
            if(phys < 0.0f) phys = 0.0f;
            int f0 = (int)phys;
            /* outer-code parity on the air (the Flipper's green LED): no data, the line waits; the
             * "Verifying" pill says so, mid-pass and after the last data (until END) */
            anim_repairing = sstv_pass_frame_kind(f0, df, intro) != SSTV_PASS_DATA;
            float fr = phys - (float)f0;
            int d0 = sstv_pass_data_before(f0, df, intro), d1 = sstv_pass_data_before(f0 + 1, df, intro);
            float bytes = ((float)d0 + (float)(d1 - d0) * fr - (float)SSTV_ILEAVE) * (float)SSTV_NOTE_CHUNK;
            /* last decoded row whose bytes are in (row_pos only grows along g) */
            int lo = 0, hi = decoded; /* rows [0, lo) are in */
            while(lo < hi) {
                int mid = (lo + hi) / 2;
                if((float)anim_rowpos[mid] <= bytes) lo = mid + 1;
                else hi = mid;
            }
            target = (float)lo;
            if(lo < decoded) { /* part of the way into the next row, by bytes */
                float p0 = lo > 0 ? (float)anim_rowpos[lo - 1] : (float)anim_hdr_len, p1 = (float)anim_rowpos[lo];
                if(p1 > p0) target += (bytes - p0) / (p1 - p0) > 0.0f ? (bytes - p0) / (p1 - p0) : 0.0f;
            }
            if(target > (float)decoded) target = (float)decoded;
        }
        if(!rx_paused && anim_line < target) {
            float d = target - anim_line, stepr = d * 0.2f;
            if(stepr < 0.25f) stepr = 0.25f;
            anim_line = d < stepr ? target : anim_line + stepr;
        }
        bool caught_up = (finished || sstv_anim_dec_done(&anim_dec)) && anim_line >= (float)decoded;
        if(caught_up) {
            unsigned step = 1000u / (unsigned)(a->fps > 0 ? a->fps : 2);
            if(enow - anim_play_ms > 5000) anim_play_ms = enow; /* (re)start */
            while(enow - anim_play_ms >= step) {
                anim_play_ms += step;
                anim_play_pos = (anim_play_pos + 1) % a->order_len;
            }
            show = a->order[anim_play_pos];
            fsrc = anim_frames + show * fb;
        } else {
            int g = (int)anim_line;
            int cur = g / H, rows = g % H;
            if(cur >= a->n) {
                cur = a->n - 1;
                rows = H;
            }
            if(!anim_compose) anim_compose = pd->system->realloc(NULL, (size_t)fb);
            int stride = sstv_anim_stride(a->w);
            memcpy(anim_compose, anim_frames + cur * fb, (size_t)(rows * stride));
            if(cur > 0) memcpy(anim_compose + rows * stride, anim_frames + (cur - 1) * fb + rows * stride, (size_t)((H - rows) * stride));
            else memset(anim_compose + rows * stride, 0, (size_t)((H - rows) * stride));
            fsrc = anim_compose;
            show = cur;
            if(rows < H) row = rows;
            anim_play_ms = enow;
        }
        xbm_to_bitmap(fsrc, a->w, a->h, anim_bm);
        pd->graphics->drawScaledBitmap(anim_bm, ox, oy, (float)sc, (float)sc);
        pd->graphics->drawRect(ox - 2, oy - 2, a->w * sc + 4, a->h * sc + 4, kColorBlack);
        if(row >= 0) { /* the drawing line: markers at both ends, roots hanging below (as on pictures) */
            int ly = oy + row * sc;
            int fw = a->w * sc;
            pd->graphics->fillRect(ox, ly, fw, 1, kColorBlack);
            pd->graphics->fillRect(ox - 6, ly - 2, 4, 5, kColorBlack);
            pd->graphics->fillRect(ox + fw + 2, ly - 2, 4, 5, kColorBlack);
            int bottom = oy + a->h * sc;
            for(int rx0 = 2; rx0 < fw; rx0 += 6) {
                float ph = (float)now / 130.0f + (float)rx0 * 0.5f;
                int len = 2 + (int)(2.5f * (0.5f + 0.5f * sinf(ph)));
                if((rx0 / 6) & 1) len += 1;
                if(ly + 1 + len > bottom) len = bottom - ly - 1;
                if(len > 0) pd->graphics->fillRect(ox + rx0, ly + 1, 1, len, kColorBlack);
            }
        }
        anim_shown = show;
        y = LCD_ROWS; /* keeps the title bar down: frame number and progress live there */
        frontier = 0;
        goto chrome;
    }
    if(rx.anim) {
        /* animation without its header yet (the first data block was lost and waits for the
         * outer-code repair, or the stream is invalid): the scene, not an empty note */
        draw_scene();
        y = LCD_ROWS;
        frontier = 0;
        goto chrome;
    }
    if(rx.note) {
        /* a note pass: the sticky note slides out smoothly as its frames arrive, then rests */
        const int rest = BAR_H + 6;
        float frac = 0.0f;
        if(finished) {
            frac = 1.0f;
        } else if(started) {
            int textframes = rx.data_frames > SSTV_ILEAVE ? rx.data_frames - SSTV_ILEAVE : SSTV_NOTE_MAX_FRAMES;
            int elapsed = (int)((float)(enow - started_ms) * 1000.0f / (float)PHYS_US_NOW);
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
        if(nh > 24) {
            /* follow the newest text while receiving, until the crank takes over */
            if(note_follow) note_scroll = sticky_note_max_scroll(pd, font_notes, nw, nh, note_buf);
            draw_sticky_note_scrolled(pd, font_notes, nx, y, nw, nh, note_buf, &note_scroll);
        }
        goto chrome; /* skip the picture-sheet code */
    }
    /* The drawing line follows the decoded lines, not the clock: revealed eases toward
     * lines_reached and never overtakes it. A coded block decodes as a batch (lines_reached
     * jumps), then its lines are painted in one by one as revealed catches up, and the line
     * waits at the last decoded row until the next block arrives (no preview running ahead). */
    if(!rx_paused && revealed < (float)lines_reached) {
        revealed += 0.8f; /* lines per frame */
        if(revealed > (float)lines_reached) revealed = (float)lines_reached;
    }
    int shown = finished ? SSTV_HEIGHT : (int)(revealed + 0.5f);
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
        /* placeholder (preview) everywhere the paper covers, then the real picture revealed
         * only down to the drawing line, so decoded coded blocks appear line by line */
        if(ph_bm) pd->graphics->drawBitmap(ph_bm, 0, y, kBitmapUnflipped);
        int clip_h = frontier > 0 ? frontier : 0;
        pd->graphics->setClipRect(0, 0, LCD_COLUMNS, clip_h);
        pd->graphics->drawBitmap(image, 0, y, kBitmapUnflipped);
        pd->graphics->clearClipRect();
        if(y > 0) pd->graphics->fillRect(0, y, LCD_COLUMNS, 1, kColorBlack); /* paper top edge */
        if(frontier > 0 && frontier < LCD_ROWS) {
            pd->graphics->fillRect(0, frontier - 1, LCD_COLUMNS, 1, kColorBlack); /* thin drawing line */
            /* 1px roots hang down from the drawing line, alternating lengths, animated */
            for(int rx0 = 2; rx0 < LCD_COLUMNS; rx0 += 6) {
                float ph = (float)now / 130.0f + (float)rx0 * 0.5f;
                int len = 2 + (int)(2.5f * (0.5f + 0.5f * sinf(ph)));
                if((rx0 / 6) & 1) len += 1; /* alternate */
                if(frontier + len > LCD_ROWS) len = LCD_ROWS - frontier;
                if(len > 0) pd->graphics->fillRect(rx0, frontier, 1, len, kColorBlack);
            }
        }
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
        if(started || (finished && rx.anim && anim_hdr_len > 0)) {
            char nm[NAME_CAP], bar[NAME_CAP + 16];
            display_name(nm, sizeof nm, current_name);
            if(rx.anim && anim_hdr_len > 0) fit_text(bar, sizeof bar, nm, 170); /* room for the counter and bar */
            else snprintf(bar, sizeof bar, "Receiving: \"%s\"", nm);
            draw_title_bar(bar, NULL);
            if(rx.anim && anim_hdr_len > 0) { /* frame counter + progress bar, left of the LED */
                char fc[24];
                snprintf(fc, sizeof fc, "%d/%d", anim_shown + 1, anim_info.n);
                const int bw = 64, bh = 8, right = LCD_COLUMNS - 34;
                float frac = pass_progress(enow);
                if(frac >= 0.0f) draw_mini_bar(right - bw, (BAR_H - bh) / 2, bw, bh, frac);
                use_font(font_ui);
                draw_text_white(fc, right - bw - 8 - text_width(fc), (BAR_H - font_height()) / 2);
            }
        } else {
            draw_title_bar(mic_state == 1 ? "Listening" : mic_state < 0 ? "No microphone" : "Microphone?", NULL);
        }
        pd->graphics->setDrawOffset(0, 0);
    }
    bool verifying = started && !finished && !rx_paused && !rx.note && (int)revealed >= SSTV_HEIGHT;
    if(rx.anim && anim_hdr_len > 0 && started && !finished && !rx_paused && anim_repairing) verifying = true;
    /* The progress pill appears once the scene's own preview bar is gone (the picture lines have
     * started; a note has no preview bar) and stays until "Verifying" takes its corner. */
    if(started && !finished && !rx_paused && !verifying && !(rx.anim && anim_hdr_len > 0) && (rx.note || segs_done > 0))
        draw_progress_pill(now, enow);
    /* Picture fully in but the pass is not done yet: the outer-code parity is still arriving.
     * Show a spinner and "Verifying" until END (or the timeout) finishes the pass. */
    if(verifying) {
        use_font(font_ui);
        const char* v = "Verifying";
        int r = 6, sp = 2 * r, pad = 10, gap = 6, tw = text_width(v);
        int w = pad + sp + gap + tw + pad, h = 24, bx = LCD_COLUMNS - w - 10, by = LCD_ROWS - h - 10;
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
    draw_rec_status();
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

static const char* const gallery_title[4] = {"Gallery", "Images", "Animations", "Notes"};
static void draw_grid(void) {
    pd->graphics->clear(kColorWhite);
    char t[48], nm[NAME_CAP];
    int pages = (gallery_count + GRID_PER_PAGE - 1) / GRID_PER_PAGE;
    if(pages > 1)
        snprintf(t, sizeof t, "%s  %d  -  %d/%d", gallery_title[gallery_filter], gallery_count, gallery_page / GRID_PER_PAGE + 1, pages);
    else
        snprintf(t, sizeof t, "%s  %d", gallery_title[gallery_filter], gallery_count);
    draw_title_bar(t, "Ⓐ open   Ⓑ back");
    use_font(font_ui);
    if(gallery_count == 0) {
        static const char* const empty[4] = {"nothing saved yet", "no images yet", "no animations yet", "no notes yet"};
        draw_text_centered(empty[gallery_filter], LCD_COLUMNS / 2, 110);
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
        use_font(font_ui);
        fit_text(t, sizeof t, nm, cell_w - 12);
        draw_text_centered(t, (k % GRID_COLS) * cell_w + cell_w / 2, cy + THUMB_H + 9);
    }
}

static void draw_full(void) {
    pd->graphics->clear(kColorWhite);
    if(full_is_anim) {
        unsigned now = pd->system->getCurrentTimeMilliseconds();
        unsigned step = 1000u / (unsigned)(full_anim.fps > 0 ? full_anim.fps : 2);
        while(now - anim_play_ms >= step) {
            anim_play_ms += step;
            anim_play_pos = (anim_play_pos + 1) % full_anim.order_len;
        }
        LCDBitmap* one = pd->graphics->newBitmap(full_anim.w, full_anim.h, kColorWhite);
        xbm_to_bitmap(full_anim_frames + full_anim.order[anim_play_pos] * sstv_anim_frame_bytes(&full_anim), full_anim.w, full_anim.h, one);
        int sc = fit_scale(full_anim.w, full_anim.h, LCD_COLUMNS, LCD_ROWS);
        pd->graphics->drawScaledBitmap(one, (LCD_COLUMNS - full_anim.w * sc) / 2, (LCD_ROWS - full_anim.h * sc) / 2, (float)sc, (float)sc);
        pd->graphics->freeBitmap(one);
    } else if(full_is_note)
        draw_sticky_note_scrolled(pd, font_notes, 24, 36, LCD_COLUMNS - 48, LCD_ROWS - 72, full_note_text, &full_scroll); /* leave room for the top/bottom hint pills; crank scrolls */
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
        const char* msg = full_is_anim ? "Delete this animation?" : full_is_note ? "Delete this note?" : "Delete this image?";
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

/* System-menu items for the screen: "filter" only in the gallery (and in About opened from it),
 * "about" and "support" everywhere. Rebuilt when that changes; the filter keeps its value. */
static void sync_menu_items(void) {
    static int shown = -1; /* -1 none yet, 0 about + support, 1 filter + about + support */
    bool in_gallery = ui_mode == ModeGrid || ui_mode == ModeFull ||
                      (ui_mode == ModeAbout && (about_back == ModeGrid || about_back == ModeFull));
    if(shown == (int)in_gallery) return;
    shown = in_gallery;
    pd->system->removeAllMenuItems();
    filter_item = NULL;
    if(in_gallery) {
        static const char* show[4] = {"all", "images", "anims", "notes"};
        filter_item = pd->system->addOptionsMenuItem("filter", show, 4, filter_menu_cb, NULL);
        pd->system->setMenuItemValue(filter_item, gallery_filter);
    }
    pd->system->addMenuItem("About", about_menu_cb, (void*)0);
    pd->system->addMenuItem("Support", about_menu_cb, (void*)1);
}

static int update(void* userdata) {
    (void)userdata;
    sync_menu_items();

    PDButtons pushed;
    pd->system->getButtonState(NULL, &pushed, NULL);

    int16_t buf[1024];
    int n;
    /* At most MAX_PER_UPDATE samples per frame: if decoding ever falls behind real time, the
     * rest waits in the ring (and overflows into mic_dropped) instead of stalling the loop. */
    int budget = 8192;
    pd->system->resetElapsedTime();
    int dsp_n = 0;
    while(budget > 0 && (n = mic_ring_read(&ring, buf, budget < 1024 ? budget : 1024)) > 0) {
        budget -= n;
        dsp_n += n;
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
    /* DSP load: time spent decoding vs the audio's own duration, averaged over ~1 s */
    dsp_time += pd->system->getElapsedTime();
    dsp_samples += dsp_n;
    if(dsp_samples >= 44100) {
        dsp_load = (int)(dsp_time * 44100.0f * 100.0f / (float)dsp_samples + 0.5f);
        static int dsp_ticks;
        if(++dsp_ticks % 5 == 0 && mic_running) rxlog("S dsp=%d%% dropped=%u in=%d", dsp_load, (unsigned)mic_dropped, rx.frames_in);
        dsp_time = 0.0f;
        dsp_samples = 0;
    }
    if(preview_dirty) { /* coalesce a block's 4 chunks into one refresh so the scene does not hitch */
        preview_refresh_bitmap();
        for(int line = 0; line < SSTV_HEIGHT; line++) paint_placeholder_line(line);
        preview_dirty = false;
    }

    unsigned now = pd->system->getCurrentTimeMilliseconds();
    if(now - log_flush_ms > 2000) {
        log_flush();
        log_flush_ms = now;
    }
    if(chime_at && now >= chime_at) { /* the Playdate's half of the end-of-pass duet */
        chime_at = 0;
        uint32_t t = pd->sound->getCurrentTime();
        uint32_t step = (uint32_t)(CHIME_NOTE_S * 44100.0f);
        if(chime_synth[0] && chime_synth[1]) {
            pd->sound->synth->playNote(chime_synth[0], chime_ok ? 1568.0f : 1244.5f, 0.7f, CHIME_NOTE_S, t); /* G6 / Eb6 */
            pd->sound->synth->playNote(chime_synth[1], chime_ok ? 2093.0f : 1046.5f, 0.7f, CHIME_NOTE_S, t + step); /* C7 / C6 */
        }
    }
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
    if(rx.anim && started && !finished) anim_pump(4096, false); /* ~200k pixels/s, far ahead of the air */
    /* END never came: the pass is over by the announced timeline plus a margin. */
    if(started && total_frames > 0 && now - started_ms > (unsigned)(total_frames - SSTV_ILEAVE) * (PHYS_US_NOW / 1000) + 4000) finish(now);

    switch(ui_mode) {
    case ModeAbout: /* receiving goes on underneath; a new pass still takes over the screen */
        if(pushed & (kButtonA | kButtonB)) ui_mode = about_back;
        draw_about();
        break;
    case ModeMenu:
        if(pushed & (kButtonLeft | kButtonRight)) menu_index ^= 1;
        if(pushed & kButtonA) {
            if(menu_index == 0) {
                ui_mode = ModeReceive; /* the microphone runs only here */
                confirm_leave = false;
                clear_image(); /* each visit to Receive starts a fresh listen */
                ask_mic();
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
        if(pushed & kButtonA) {
            clear_image();
            sstv_rx_forget(&rx); /* clear means clear: no merging into what was on screen */
        }
        if(pushed & kButtonB) {
            if(started && !finished) confirm_leave = true; /* a pass is in progress: ask first */
            else menu_open();
        }
        if(pushed & kButtonRight) show_panel = !show_panel;
        { /* crank scrolls a note */
            float dc = pd->system->getCrankChange();
            if(rx.note && (dc > 0.5f || dc < -0.5f)) { note_scroll += (int)(dc * 0.6f); note_follow = false; }
        }
        if((pushed & kButtonUp) && rec_len < 0 && rec_buf) {
            rec_prefix = "rec";
            rec_len = 0;
        }
        if((pushed & kButtonDown) && rec_len < 0 && rec_buf) { /* temporary: tone test takes */
            rec_prefix = "test";
            rec_len = 0;
        }
        if(calib_active && now - calib_last_ms > 3000) calib_active = false;
        if(calib_active) draw_calibration();
        else draw_receive();
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
                if(gallery_index >= 0 && gallery_index < gallery_count) {
                    pd->file->unlink(gallery_names[gallery_index], 0);
                    sstv_rx_forget(&rx); /* a resend of what was deleted is received from scratch */
                    /* and its computer copy: picture.png / animation.gif */
                    char pair[NAME_CAP + 4];
                    strncpy(pair, gallery_names[gallery_index], sizeof pair - 1);
                    pair[sizeof pair - 1] = 0;
                    char* dot = strrchr(pair, '.');
                    if(dot && gallery_kind[gallery_index] != 1) {
                        strcpy(dot, gallery_kind[gallery_index] == 2 ? ".gif" : ".png");
                        pd->file->unlink(pair, 0);
                    }
                }
                confirm_delete = false;
                gallery_scan(gallery_filter); /* refresh the list and thumbnails */
                if(gallery_count == 0) ui_mode = ModeGrid; /* the filter's "nothing here" */
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
        { /* crank scrolls a note */
            float dc = pd->system->getCrankChange();
            if(full_is_note && (dc > 0.5f || dc < -0.5f)) full_scroll += (int)(dc * 0.6f);
        }
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
    font_notes = pd->graphics->loadFont("fonts/Notes-14", &err);
    if(!font_glyph) font_glyph = font_ui;
    if(!font_notes) font_notes = font_ui;
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
    qr_github = pd->graphics->loadBitmap("images/qr_github", &err);
    qr_coffee = pd->graphics->loadBitmap("images/qr_coffee", &err);
    bmc_button = pd->graphics->loadBitmap("images/bmc_button", &err);

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
    export_realloc = pd->system->realloc;
    log_open();
    rxlog("LAUNCH");
    mic_ring_init(&ring);
    sstv_rx_init(&rx, on_pixels, on_meta, on_preview, NULL);
    sstv_rx_set_control_cb(&rx, on_control);
    sstv_rx_set_note_cb(&rx, on_note);
    sstv_demod_init(&demod, 44100.0f, on_phys, NULL);
    sstv_demod_set_band(&demod, SSTV_BAND_HF);
    for(int i = 0; i < 2; i++) {
        chime_synth[i] = pd->sound->synth->newSynth();
        pd->sound->synth->setWaveform(chime_synth[i], kWaveformSquare);
        pd->sound->synth->setAttackTime(chime_synth[i], 0.005f);
        pd->sound->synth->setReleaseTime(chime_synth[i], 0.04f);
        pd->sound->channel->addSource(pd->sound->getDefaultChannel(), (SoundSource*)chime_synth[i]);
        start_synth[i] = pd->sound->synth->newSynth();
        pd->sound->synth->setWaveform(start_synth[i], kWaveformSine);
        pd->sound->synth->setAttackTime(start_synth[i], 0.005f);
        pd->sound->synth->setReleaseTime(start_synth[i], 0.04f);
        pd->sound->channel->addSource(pd->sound->getDefaultChannel(), (SoundSource*)start_synth[i]);
    }
    clear_image();
    menu_open(); /* the microphone is asked for when Receive opens (ask_mic) */
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
            log_flush();
        }
    } else if(event == kEventUnlock) {
        if(pd) start_mic();
    }
    return 0;
}
