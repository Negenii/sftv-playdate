#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sstv_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header helpers. */
static inline uint16_t sstv_hdr(int type, int index) {
    return (uint16_t)((type << SSTV_FT_SHIFT) | (index & SSTV_INDEX_MASK));
}
static inline int sstv_hdr_type(uint16_t hdr) {
    return hdr >> SSTV_FT_SHIFT;
}
static inline int sstv_hdr_index(uint16_t hdr) {
    return hdr & SSTV_INDEX_MASK;
}

/* Logical frame: out[SSTV_FRAME_BYTES] = header (big endian), SSTV_PAYLOAD_BYTES payload,
 * CRC-8 over those, then Reed-Solomon parity. */
void sstv_frame_pack(uint16_t hdr, const uint8_t* payload, uint8_t* out);

/* Decodes a logical frame. byte_conf (may be NULL): per-byte confidence in [0,1]; the
 * least confident bytes are tried as erasures (0, 2, 4, ... up to SSTV_PARITY_BYTES) until
 * RS + CRC agree. Returns true on success; hdr/payload are filled either way (raw when
 * decoding failed). *corrected (may be NULL): bytes fixed, or -1 when RS gave up. */
bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* hdr, uint8_t* payload, int* corrected);

static inline bool sstv_frame_unpack(const uint8_t* in, uint16_t* hdr, uint8_t* payload) {
    return sstv_frame_unpack_soft(in, NULL, hdr, payload, NULL);
}

/* Bit `index` (0..SSTV_FRAME_BITS-1) of a packed frame, MSB first within each byte. */
static inline int sstv_frame_bit(const uint8_t* frame, int index) {
    return (frame[index >> 3] >> (7 - (index & 7))) & 1;
}

/* Symbol `index` (0..SSTV_SYMBOLS_PER_FRAME-1): SSTV_BITS_PER_SYMBOL consecutive bits,
 * first bit most significant. */
static inline int sstv_frame_symbol(const uint8_t* frame, int index) {
    int v = 0;
    for(int b = 0; b < SSTV_BITS_PER_SYMBOL; b++) {
        v = (v << 1) | sstv_frame_bit(frame, index * SSTV_BITS_PER_SYMBOL + b);
    }
    return v;
}

/* Symbol `index` with `bits` bits per symbol (the HF band uses 3); bits past SSTV_FRAME_BITS
 * (padding of the last symbol) read as 0. */
static inline int sstv_frame_symbol_bits(const uint8_t* frame, int index, int bits) {
    int v = 0;
    for(int b = 0; b < bits; b++) {
        int bi = index * bits + b;
        v = (v << 1) | (bi < SSTV_FRAME_BITS ? sstv_frame_bit(frame, bi) : 0);
    }
    return v;
}

/* Byte interleaving of SSTV_ILEAVE logical frames into SSTV_ILEAVE physical frames:
 * physical stream position j*SSTV_ILEAVE + i holds byte j of logical frame i. Both
 * buffers are SSTV_ILEAVE * SSTV_FRAME_BYTES. */
void sstv_interleave(const uint8_t* logical, uint8_t* physical);
void sstv_deinterleave(const uint8_t* physical, uint8_t* logical);
void sstv_deinterleave_conf(const float* physical_conf, float* logical_conf);

/* Segment geometry (raw mode). */
static inline int sstv_seg_line(int seg) {
    return seg / SSTV_SEGS_PER_LINE;
}
static inline int sstv_seg_byte_offset(int seg) {
    return (seg % SSTV_SEGS_PER_LINE) * SSTV_SEG_BYTES;
}

/* Pass layout (revision 7). Data position = order of the logical frames in a revision 6 pass
 * without END: START 0..3, TINY 4..7, PREVIEW 8..27, SCHEDULE 28..31 (coded only), RAW
 * segment s at 28 + s, CODED block with header index b, frame i at (b + 1) * 4 + i. Groups of
 * SSTV_OUTER_GROUP positions are each followed by SSTV_OUTER_PARITY parity frames; END last. */
#define SSTV_PASS_DATA 0
#define SSTV_PASS_PARITY 1
#define SSTV_PASS_END 2
int sstv_pass_position(uint16_t hdr, int i); /* -1 for META (use the meta variant) and PARITY */
int sstv_pass_meta_position(int subtype, int index, int i); /* -1 for END */

/* Grouping (revision 7.1): group 0 is the intro (START + previews + schedule) so its parity
 * arrives right after it, while the console scene is still up; groups 1+ are SSTV_OUTER_GROUP
 * picture frames each. `intro` is the group-0 size = SSTV_ILEAVE + SSTV_INTRO_FRAMES[_CODED],
 * from sstv_pass_intro(coded). */
static inline int sstv_pass_intro(int coded) {
    return SSTV_ILEAVE + (coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES);
}
int sstv_pass_groups(int data_frames, int intro);
int sstv_pass_group_size(int data_frames, int intro, int g);
int sstv_pass_group_base(int intro, int g); /* first data position of group g */
int sstv_pass_group_of(int pos, int intro); /* group holding data position pos */
int sstv_pass_total(int data_frames, int intro); /* physical frames of the whole pass */
int sstv_pass_frame_kind(int phys, int data_frames, int intro);
int sstv_pass_data_before(int phys, int data_frames, int intro); /* data frames among physical frames < phys */

/* Coded block payload: [line count][first line#][codec record]... consecutive lines,
 * zero padded. */
int sstv_block_pack(const uint8_t* image_bits, int first_line, uint8_t* payload);
typedef void (*sstv_block_line_cb)(void* ctx, uint16_t line, const uint8_t* pixels);
int sstv_block_unpack(const uint8_t* payload, int valid_bytes, sstv_block_line_cb cb, void* ctx);

/* Builds every physical frame of an image: a start meta block carrying `name`, then either raw
 * segments or coded blocks, whichever is shorter by SSTV_CODED_MIN_GAIN, then an end meta block. out must hold
 * SSTV_MAX_FRAMES * SSTV_FRAME_BYTES bytes. lines_after (may be NULL, SSTV_MAX_FRAMES
 * entries) receives, for every physical frame, how many image lines are complete once that
 * frame has been sent (for progress display). Returns the number of physical frames
 * (a multiple of SSTV_ILEAVE); *coded (may be NULL) tells which mode was chosen. */
/* Preamble (see SSTV_LEADER_SYMBOLS / SSTV_LOCK_FRAMES in sstv_params.h). */
static inline int sstv_leader_symbol(int i) { /* symbol i of the start ladder */
    const int per = SSTV_LADDER_TONE_SYMBOLS + SSTV_LADDER_GAP_SYMBOLS;
    return (i % per) < SSTV_LADDER_TONE_SYMBOLS ? 1 + i / per : 0;
}
void sstv_lock_frame(uint8_t* out); /* SSTV_FRAME_BYTES of the lock-frame pattern */
bool sstv_is_lock_frame(const uint8_t* frame); /* a received lock frame (receivers drop it) */
int sstv_lock_distance(const uint8_t* frame); /* bits (of SSTV_FRAME_BITS) that differ from the lock frame */

int sstv_build_physical(const uint8_t* image_bits, const char* name, uint8_t* out, uint8_t* lines_after, bool* coded);

/* Builds every physical frame of a text note: START (note flag) + name, then the ASCII text as
 * RAW frames (SSTV_NOTE_CHUNK bytes each, header index = chunk), all in one outer-code group
 * with its parity, then END. `text` is NUL-terminated ASCII, truncated to SSTV_NOTE_MAX_CHARS.
 * out holds SSTV_MAX_FRAMES * SSTV_FRAME_BYTES. lines_after (may be NULL) receives, per physical
 * frame, how many text chunks are complete once that frame is out. Returns the physical frame
 * count (a multiple of SSTV_ILEAVE). */
int sstv_build_note(const char* text, const char* name, uint8_t* out, uint8_t* lines_after);

/* An animation stream (sstv_anim.h, up to SSTV_NOTE_MAX_BYTES) in the note layout with the
 * animation flags. out holds sstv_blob_frames(len) * SSTV_FRAME_BYTES, lines_after (may be NULL)
 * as many bytes. Returns the physical frame count. */
int sstv_build_anim(const uint8_t* data, int len, const char* name, uint8_t* out, uint8_t* lines_after);
/* Physical frames of a note-layout pass carrying `len` bytes. */
int sstv_blob_frames(int len);

/* The same pass built one physical block (SSTV_ILEAVE interleaved frames) at a time, for a
 * transmitter without room for the whole pass: only the outer-code parity is computed up front
 * (sstv_blob_parity_bytes(len), ~312 bytes per 108 frames). Block b of the pass equals block b
 * of sstv_build_anim / sstv_build_note output. `data` must stay valid while blocks are built. */
typedef struct {
    const uint8_t* data;
    int len;
    uint8_t meta[SSTV_PAYLOAD_BYTES]; /* START payload (END = the same with the END subtype) */
    int data_frames, intro, groups, total; /* total = physical frames */
    uint8_t* parity; /* groups * SSTV_OUTER_PARITY * SSTV_PAYLOAD_BYTES */
} sstv_blob;
int sstv_blob_parity_bytes(int len);
void sstv_blob_init(sstv_blob* b, const uint8_t* data, int len, bool anim, const char* name, uint8_t* parity_mem);
void sstv_blob_block(const sstv_blob* b, int block, uint8_t* phys);

#ifdef __cplusplus
}
#endif
