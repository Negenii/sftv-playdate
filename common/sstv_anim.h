#pragma once
/* Animation codec (revision 12, stream version 3): a Flipper animation (frame_N.bm + meta.txt)
 * at its own size, sent as a blob pass (see sstv_build_anim). Every pixel is coded with a binary
 * range coder (LZMA style, 12-bit probabilities) whose probability comes from a context of 6
 * already coded neighbours in the frame and 5 pixels around the same spot in a reference frame.
 * One model runs through the whole stream and only frame 0 is coded alone: the reference of
 * every later frame is the most similar of the SSTV_ANIM_SEG - 1 frames before it (Hamming
 * distance), and an identical one makes it a duplicate with no pixels. (Version 2 reset the
 * model and coded a key frame every 16 frames so a hole cost one segment; with resends merged
 * by the receiver that bought little and cost 11 % of the size.) Measured on the stock
 * animations: Kaiju 32.4 KB as .bm -> 11.0 KB, Freedom_2_dolphins 49 -> 17.5 KB.
 *
 * Stream: [version][w][h][n][fps][passive][active][order_len][order...][ref x n]
 *         [checkpoint offsets, u16 LE x segs, from the end of the header]
 *         [checkpoint CRC-16/CCITT-FALSE, u16 LE x segs][coded bytes...]
 * Checkpoint s covers the bytes from where frame 16 s starts to where frame 16 (s + 1) starts.
 * The CRCs catch a false-accepted frame (one that passed the frame CRC-8 by chance): the receiver
 * checks each range once its bytes are in, and after a mismatch decodes again from the start
 * once the bytes change (a resend of the same item fills and overwrites them).
 * ref: SSTV_ANIM_KEY (coded alone), j (coded against frame j) or SSTV_ANIM_DUP | j (copy of j).
 * Frames are XBM rows: stride (w + 7) / 8, leftmost pixel in bit 0, 1 = black. */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SSTV_ANIM_VERSION 3
#define SSTV_ANIM_MAX_FRAMES 120
#define SSTV_ANIM_MAX_ORDER 255
#define SSTV_ANIM_SEG 16
#define SSTV_ANIM_MAX_SEGS ((SSTV_ANIM_MAX_FRAMES + SSTV_ANIM_SEG - 1) / SSTV_ANIM_SEG)
#define SSTV_ANIM_KEY 0x7F
#define SSTV_ANIM_DUP 0x80
#define SSTV_ANIM_CTX (1 << 12)
#define SSTV_ANIM_FIXED 8 /* header bytes before the order list */

typedef struct {
    int w, h, n, fps, passive, active, order_len;
    uint8_t order[SSTV_ANIM_MAX_ORDER];
    uint8_t ref[SSTV_ANIM_MAX_FRAMES]; /* set by the encoder */
    uint16_t seg_off[SSTV_ANIM_MAX_SEGS]; /* set by the encoder */
    uint16_t seg_crc[SSTV_ANIM_MAX_SEGS]; /* set by the encoder */
    uint16_t frame_end[SSTV_ANIM_MAX_FRAMES]; /* encoder only, not sent: stream bytes written once
                                                 frame i is coded (progress display) */
} sstv_anim_info;

static inline int sstv_anim_stride(int w) {
    return (w + 7) / 8;
}
static inline int sstv_anim_frame_bytes(const sstv_anim_info* a) {
    return sstv_anim_stride(a->w) * a->h;
}
static inline int sstv_anim_segs(int n) {
    return (n + SSTV_ANIM_SEG - 1) / SSTV_ANIM_SEG;
}
static inline int sstv_anim_header_len(const sstv_anim_info* a) {
    return SSTV_ANIM_FIXED + a->order_len + a->n + 4 * sstv_anim_segs(a->n);
}

/* ---- encoder (the Flipper) ---- */

/* Frame `index` into `bits` (frame_bytes). False on a read error. */
typedef bool (*sstv_anim_get_frame)(void* ctx, int index, uint8_t* bits);
/* Scratch the encoder needs: a window of SSTV_ANIM_SEG frames plus the model. */
static inline int sstv_anim_scratch_bytes(const sstv_anim_info* a) {
    return SSTV_ANIM_SEG * sstv_anim_frame_bytes(a) + SSTV_ANIM_CTX * 2;
}
/* a: w, h, n, fps, passive, active, order filled in (ref and seg_off are written). Returns the
 * stream length, -1 when it does not fit `cap` or a frame could not be read, -2 on bad info.
 * progress (may be NULL) gets the number of frames coded so far. */
int sstv_anim_encode(
    sstv_anim_info* a, sstv_anim_get_frame get, void* ctx, uint8_t* scratch, uint8_t* out, int cap,
    void (*progress)(void* pctx, int frames_done), void* pctx);

/* ---- decoder (receivers) ---- */

/* Stream byte `pos`, or -1 while it has not arrived. */
typedef int (*sstv_anim_byte)(void* ctx, int pos);

/* Parses the header once its bytes are in. Returns the header length, 0 while bytes are
 * missing, -1 when it is not a valid version-1 stream. */
int sstv_anim_parse(sstv_anim_info* a, sstv_anim_byte get, void* ctx);

typedef struct {
    sstv_anim_info info;
    int hdr_len, stream_len;
    uint8_t* frames; /* n * frame_bytes, owned by the caller */
    uint8_t state[SSTV_ANIM_MAX_FRAMES]; /* 0 nothing yet, 1 decoding/partial, 2 complete, 3 corrupt */
    int frames_done;
    int corrupt; /* times the coder ran past the end of the stream (a wrong byte) */
    uint16_t* row_pos; /* optional (set after init): stream position once row r of frame k is
                          decoded, at [k * h + r] (receivers time the drawing line with it) */
    int seg; /* checkpoint of the frame being decoded (== segs: all done or given up) */
    bool seg_open;
    int frame; /* frame being decoded */
    bool frame_open;
    int pix; /* next pixel of `frame` (row major) */
    int pos, end; /* next stream byte, end of the stream */
    uint32_t range, code;
    uint16_t prob[SSTV_ANIM_CTX];
} sstv_anim_dec;

/* frames: n * frame_bytes bytes, cleared to white here. */
void sstv_anim_dec_init(sstv_anim_dec* d, const sstv_anim_info* a, int hdr_len, int stream_len, uint8_t* frames);
/* Decodes up to `budget` pixels as far as the bytes in hand allow. A frame starts as a copy of
 * its reference (white for frame 0) and its pixels are overwritten as they decode, so a partial
 * frame already looks like the frame. final: the pass is over; decoding stops at the first
 * missing byte (the frames after it stay as they are). Returns the pixels decoded; true in
 * *stuck (may be NULL) when it stopped on a missing byte. */
int sstv_anim_dec_run(sstv_anim_dec* d, sstv_anim_byte get, void* ctx, int budget, bool final, bool* stuck);
/* Checkpoint s's byte range in the stream. */
void sstv_anim_seg_range(const sstv_anim_dec* d, int s, int* start, int* end);
/* 1: all bytes in and the CRC matches; 0: bytes still missing; -1: CRC mismatch. */
int sstv_anim_seg_check(const sstv_anim_dec* d, int s, sstv_anim_byte get, void* ctx);
/* Decode again from the start (bytes of checkpoint s changed after a failed check). Frames keep
 * their pixels until they decode again. */
void sstv_anim_dec_rewind(sstv_anim_dec* d, int s);
uint16_t sstv_anim_crc16(uint16_t crc, uint8_t b);

static inline bool sstv_anim_dec_done(const sstv_anim_dec* d) {
    return d->seg >= sstv_anim_segs(d->info.n);
}

#ifdef __cplusplus
}
#endif
