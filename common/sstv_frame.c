#include "sstv_frame.h"
#include "sstv_preview.h"
#include "sstv_codec.h"
#include "crc8.h"
#include "rs.h"
#include <string.h>

void sstv_frame_pack(uint16_t hdr, const uint8_t* payload, uint8_t* out) {
    out[0] = (uint8_t)(hdr >> 8);
    out[1] = (uint8_t)(hdr & 0xFF);
    memcpy(out + 2, payload, SSTV_PAYLOAD_BYTES);
    out[SSTV_DATA_BYTES - 1] = crc8_compute(out, SSTV_DATA_BYTES - 1);
    rs_encode(out, SSTV_DATA_BYTES, out + SSTV_DATA_BYTES);
}

static bool check(const uint8_t* buf, uint16_t* hdr, uint8_t* payload) {
    *hdr = (uint16_t)((buf[0] << 8) | buf[1]);
    memcpy(payload, buf + 2, SSTV_PAYLOAD_BYTES);
    if(crc8_compute(buf, SSTV_DATA_BYTES - 1) != buf[SSTV_DATA_BYTES - 1]) return false;
    int type = sstv_hdr_type(*hdr), index = sstv_hdr_index(*hdr);
    if(type == SSTV_FT_RAW) return index < SSTV_NUM_SEGS;
    if(type == SSTV_FT_CODED) return index < SSTV_MAX_BLOCKS;
    if(type == SSTV_FT_PARITY) return index < SSTV_OUTER_GROUPS_MAX * SSTV_OUTER_PARITY;
    return type == SSTV_FT_META;
}

bool sstv_frame_unpack_soft(
    const uint8_t* in, const float* byte_conf, uint16_t* hdr, uint8_t* payload, int* corrected) {
    uint8_t buf[SSTV_FRAME_BYTES];
    int order[SSTV_FRAME_BYTES];
    int n_order = 0;
    if(byte_conf) {
        /* Least confident bytes first (insertion sort; the frame is small). */
        for(int i = 0; i < SSTV_FRAME_BYTES; i++) {
            int j = n_order++;
            while(j > 0 && byte_conf[order[j - 1]] > byte_conf[i]) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
    }
    for(int e = 0; e <= (byte_conf ? SSTV_PARITY_BYTES : 0); e += 2) {
        memcpy(buf, in, SSTV_FRAME_BYTES);
        int c = rs_decode_erasures(buf, SSTV_FRAME_BYTES, order, e);
        if(c >= 0 && check(buf, hdr, payload)) {
            if(corrected) *corrected = c;
            return true;
        }
    }
    (void)check(in, hdr, payload);
    if(corrected) *corrected = -1;
    return false;
}

void sstv_interleave(const uint8_t* logical, uint8_t* physical) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            physical[j * SSTV_ILEAVE + i] = logical[i * SSTV_FRAME_BYTES + j];
        }
    }
}

void sstv_deinterleave(const uint8_t* physical, uint8_t* logical) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            logical[i * SSTV_FRAME_BYTES + j] = physical[j * SSTV_ILEAVE + i];
        }
    }
}

void sstv_deinterleave_conf(const float* physical_conf, float* logical_conf) {
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
            logical_conf[i * SSTV_FRAME_BYTES + j] = physical_conf[j * SSTV_ILEAVE + i];
        }
    }
}

int sstv_block_pack(const uint8_t* image_bits, int first_line, uint8_t* payload) {
    memset(payload, 0, SSTV_BLOCK_PAYLOAD);
    payload[1] = (uint8_t)first_line;
    int pos = 2, count = 0;
    const uint8_t* ref = NULL;
    uint8_t rec[SSTV_CODEC_MAX_RECORD];
    for(int y = first_line; y < SSTV_HEIGHT; y++) {
        const uint8_t* row = image_bits + y * SSTV_LINE_BYTES;
        int n = sstv_codec_encode(row, ref, rec);
        if(pos + n > SSTV_BLOCK_PAYLOAD) break;
        memcpy(payload + pos, rec, (size_t)n);
        pos += n;
        count++;
        ref = row;
    }
    payload[0] = (uint8_t)count;
    return count;
}

int sstv_block_unpack(const uint8_t* payload, int valid_bytes, sstv_block_line_cb cb, void* ctx) {
    if(valid_bytes < 2) return 0;
    int count = payload[0];
    int line = payload[1];
    int pos = 2, delivered = 0;
    uint8_t prev[SSTV_LINE_BYTES], row[SSTV_LINE_BYTES];
    const uint8_t* ref = NULL;
    for(int k = 0; k < count && line + k < SSTV_HEIGHT; k++) {
        if(pos + 1 > valid_bytes) break;
        int n = sstv_codec_decode(payload + pos, SSTV_BLOCK_PAYLOAD - pos, ref, row);
        if(n < 0 || pos + n > valid_bytes) break;
        pos += n;
        if(cb) cb(ctx, (uint16_t)(line + k), row);
        delivered++;
        memcpy(prev, row, SSTV_LINE_BYTES);
        ref = prev;
    }
    return delivered;
}

int sstv_pass_position(uint16_t hdr, int i) {
    int type = sstv_hdr_type(hdr), index = sstv_hdr_index(hdr);
    if(type == SSTV_FT_RAW) return SSTV_ILEAVE + SSTV_INTRO_FRAMES + index;
    if(type == SSTV_FT_CODED) return (index + 1) * SSTV_ILEAVE + i;
    return -1;
}

int sstv_pass_meta_position(int subtype, int index, int i) {
    switch(subtype) {
    case SSTV_META_START: return i;
    case SSTV_META_TINY: return SSTV_ILEAVE + index;
    case SSTV_META_PREVIEW: return SSTV_ILEAVE + SSTV_TINY_CHUNKS + index;
    case SSTV_META_SCHEDULE: return SSTV_ILEAVE + SSTV_INTRO_FRAMES + index;
    default: return -1;
    }
}

int sstv_pass_groups(int data_frames, int intro) {
    if(data_frames <= intro) return 1;
    return 1 + (data_frames - intro + SSTV_OUTER_GROUP - 1) / SSTV_OUTER_GROUP;
}

int sstv_pass_group_size(int data_frames, int intro, int g) {
    if(g == 0) return data_frames < intro ? data_frames : intro;
    int start = intro + (g - 1) * SSTV_OUTER_GROUP;
    int left = data_frames - start;
    if(left <= 0) return 0;
    return left < SSTV_OUTER_GROUP ? left : SSTV_OUTER_GROUP;
}

int sstv_pass_group_base(int intro, int g) {
    return g == 0 ? 0 : intro + (g - 1) * SSTV_OUTER_GROUP;
}

int sstv_pass_group_of(int pos, int intro) {
    return pos < intro ? 0 : 1 + (pos - intro) / SSTV_OUTER_GROUP;
}

int sstv_pass_total(int data_frames, int intro) {
    return data_frames + sstv_pass_groups(data_frames, intro) * SSTV_OUTER_PARITY + SSTV_ILEAVE;
}

/* Walks the groups: each occupies group_size + SSTV_OUTER_PARITY physical frames. */
int sstv_pass_frame_kind(int phys, int data_frames, int intro) {
    int at = 0;
    for(int g = 0; g < sstv_pass_groups(data_frames, intro); g++) {
        int n = sstv_pass_group_size(data_frames, intro, g);
        if(phys < at + n) return SSTV_PASS_DATA;
        if(phys < at + n + SSTV_OUTER_PARITY) return SSTV_PASS_PARITY;
        at += n + SSTV_OUTER_PARITY;
    }
    return SSTV_PASS_END;
}

int sstv_pass_data_before(int phys, int data_frames, int intro) {
    int at = 0, data = 0;
    for(int g = 0; g < sstv_pass_groups(data_frames, intro); g++) {
        int n = sstv_pass_group_size(data_frames, intro, g);
        if(phys <= at + n) return data + (phys - at);
        data += n;
        at += n + SSTV_OUTER_PARITY;
        if(phys <= at) return data;
    }
    return data;
}

/* Pass emitter: writes interleaved blocks, feeds every data frame's payload to the outer
 * encoders (one per payload column) and closes a group with its parity blocks. */
typedef struct {
    uint8_t* out;
    uint8_t* lines_after;
    int blocks; /* physical blocks emitted */
    int data_frames; /* logical data frames emitted */
    rs_encoder_t enc[SSTV_PAYLOAD_BYTES];
    int group_frames; /* data frames in the open group */
    int group; /* index of the open group */
} pass_emitter;

static void emitter_init(pass_emitter* e, uint8_t* out, uint8_t* lines_after) {
    memset(e, 0, sizeof *e);
    e->out = out;
    e->lines_after = lines_after;
    for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) rs_encoder_init(&e->enc[k]);
}

static void emitter_block(pass_emitter* e, const uint8_t* logical, int lines_before, int lines_after_block) {
    sstv_interleave(logical, e->out + e->blocks * SSTV_ILEAVE * SSTV_FRAME_BYTES);
    if(e->lines_after) {
        for(int f = 0; f < SSTV_ILEAVE; f++)
            e->lines_after[e->blocks * SSTV_ILEAVE + f] = (uint8_t)(f == SSTV_ILEAVE - 1 ? lines_after_block : lines_before);
    }
    e->blocks++;
}

/* Emits the parity blocks of the open group and starts the next one. Parity frame j of group
 * g is an ordinary logical frame (type PARITY, index g * 12 + j) whose payload byte k is RS
 * parity symbol j over payload byte k of the group's frames. */
static void emitter_close_group(pass_emitter* e, int lines) {
    if(e->group_frames == 0) return;
    /* Built one block at a time straight into the logical frames (the Flipper runs this on
     * a 4 kB stack): rs_encoder_finish does not disturb the encoder state. */
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    uint8_t payload[SSTV_ILEAVE][SSTV_PAYLOAD_BYTES];
    for(int j = 0; j < SSTV_OUTER_PARITY; j += SSTV_ILEAVE) {
        for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) {
            uint8_t p[SSTV_OUTER_PARITY];
            rs_encoder_finish(&e->enc[k], p);
            for(int i = 0; i < SSTV_ILEAVE; i++) payload[i][k] = p[j + i];
        }
        for(int i = 0; i < SSTV_ILEAVE; i++)
            sstv_frame_pack(sstv_hdr(SSTV_FT_PARITY, e->group * SSTV_OUTER_PARITY + j + i), payload[i], logical + i * SSTV_FRAME_BYTES);
        emitter_block(e, logical, lines, lines);
    }
    for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) rs_encoder_init(&e->enc[k]);
    e->group_frames = 0;
    e->group++;
}

/* Emits a data block and closes the group when it is full. */
static void emitter_data_block(pass_emitter* e, const uint8_t* logical, int lines_before, int lines_after_block) {
    emitter_block(e, logical, lines_before, lines_after_block);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        const uint8_t* payload = logical + i * SSTV_FRAME_BYTES + 2;
        for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) rs_encoder_feed(&e->enc[k], payload[k]);
        e->group_frames++;
        e->data_frames++;
    }
    if(e->group_frames == SSTV_OUTER_GROUP) emitter_close_group(e, lines_after_block);
}

int sstv_build_physical(const uint8_t* image_bits, const char* name, uint8_t* out, uint8_t* lines_after, bool* coded) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    uint8_t payload[SSTV_BLOCK_PAYLOAD];

    /* How many blocks would coding need? */
    int coded_blocks = 0;
    uint8_t schedule[SSTV_MAX_BLOCKS];
    memset(schedule, 0, sizeof schedule);
    for(int line = 0; line < SSTV_HEIGHT && coded_blocks < SSTV_MAX_BLOCKS;) {
        int n = sstv_block_pack(image_bits, line, payload);
        if(n <= 0) break;
        schedule[coded_blocks] = (uint8_t)n;
        line += n;
        coded_blocks++;
    }
    int raw_frames = SSTV_NUM_SEGS;
    bool use_coded = (float)(coded_blocks * SSTV_ILEAVE) <= SSTV_CODED_MIN_GAIN * (float)raw_frames &&
                     coded_blocks <= SSTV_MAX_CODED_BLOCKS;
    if(coded) *coded = use_coded;

    pass_emitter em;
    emitter_init(&em, out, lines_after);
    /* Start meta block: [SSTV_META_START][total frames | coded flag][file name] in every
     * logical frame. Total = data frames (start + previews + data) + parity + end. */
    int data_blocks = use_coded ? coded_blocks : SSTV_NUM_SEGS / SSTV_ILEAVE;
    int intro = use_coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES;
    int data_frames = SSTV_ILEAVE + intro + data_blocks * SSTV_ILEAVE;
    uint16_t total = (uint16_t)sstv_pass_total(data_frames, sstv_pass_intro(use_coded));
    uint16_t word = total | (use_coded ? SSTV_META_CODED_FLAG : 0);
    uint8_t meta[SSTV_PAYLOAD_BYTES];
    memset(meta, 0, SSTV_PAYLOAD_BYTES);
    meta[0] = SSTV_META_START;
    meta[1] = (uint8_t)(word & 0xFF);
    meta[2] = (uint8_t)(word >> 8);
    if(name) {
        size_t n = strlen(name);
        if(n > SSTV_NAME_MAX) n = SSTV_NAME_MAX;
        memcpy(meta + SSTV_META_NAME_OFFSET, name, n);
    }
    for(int i = 0; i < SSTV_ILEAVE; i++) sstv_frame_pack(sstv_hdr(SSTV_FT_META, 0), meta, logical + i * SSTV_FRAME_BYTES);
    emitter_data_block(&em, logical, 0, 0);

    /* Tiny preview: one block of [SSTV_META_TINY][25 bytes], index = chunk. */
    uint8_t tiny[SSTV_TINY_BYTES];
    sstv_preview_make_tiny(image_bits, tiny);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        payload[0] = SSTV_META_TINY;
        memcpy(payload + 1, tiny + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK);
        sstv_frame_pack(sstv_hdr(SSTV_FT_META, i), payload, logical + i * SSTV_FRAME_BYTES);
    }
    emitter_data_block(&em, logical, 0, 0);

    /* Preview: SSTV_PREVIEW_CHUNKS independent frames [SSTV_META_PREVIEW][25 bytes], index = chunk. */
    uint8_t preview[SSTV_PREVIEW_BYTES];
    sstv_preview_make(image_bits, preview);
    for(int chunk = 0; chunk < SSTV_PREVIEW_CHUNKS; chunk += SSTV_ILEAVE) {
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            int c = chunk + i;
            memset(payload, 0, SSTV_PAYLOAD_BYTES);
            payload[0] = SSTV_META_PREVIEW;
            if(c < SSTV_PREVIEW_CHUNKS) {
                int off = c * SSTV_PREVIEW_CHUNK;
                int n = SSTV_PREVIEW_BYTES - off;
                if(n > SSTV_PREVIEW_CHUNK) n = SSTV_PREVIEW_CHUNK;
                memcpy(payload + 1, preview + off, (size_t)n);
            }
            sstv_frame_pack(sstv_hdr(SSTV_FT_META, c), payload, logical + i * SSTV_FRAME_BYTES);
        }
        emitter_data_block(&em, logical, 0, 0);
    }

    if(use_coded) {
        /* Schedule: lines per coded block, one byte each, 100 bytes in one block. */
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            payload[0] = SSTV_META_SCHEDULE;
            memcpy(payload + 1, schedule + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK);
            sstv_frame_pack(sstv_hdr(SSTV_FT_META, i), payload, logical + i * SSTV_FRAME_BYTES);
        }
        emitter_data_block(&em, logical, 0, 0);

        /* Close the intro (START + previews + schedule) as group 0 so its parity is sent now,
         * before the picture, and the receiver can repair the preview while the scene is up. */
        emitter_close_group(&em, 0);

        int line = 0;
        while(line < SSTV_HEIGHT) {
            int n = sstv_block_pack(image_bits, line, payload);
            if(n <= 0) break;
            /* header index = data block number - 1 (the first coded block is data block 8) */
            int index = em.data_frames / SSTV_ILEAVE - 1;
            for(int i = 0; i < SSTV_ILEAVE; i++) {
                sstv_frame_pack(sstv_hdr(SSTV_FT_CODED, index), payload + i * SSTV_PAYLOAD_BYTES, logical + i * SSTV_FRAME_BYTES);
            }
            line += n;
            emitter_data_block(&em, logical, line - n, line);
        }
    } else {
        emitter_close_group(&em, 0); /* intro = group 0, parity now (see the coded branch) */
        for(int seg = 0; seg < SSTV_NUM_SEGS; seg += SSTV_ILEAVE) {
            for(int i = 0; i < SSTV_ILEAVE; i++) {
                int s = seg + i;
                memset(payload, 0, SSTV_PAYLOAD_BYTES);
                memcpy(payload, image_bits + sstv_seg_line(s) * SSTV_LINE_BYTES + sstv_seg_byte_offset(s), SSTV_SEG_BYTES);
                sstv_frame_pack(sstv_hdr(SSTV_FT_RAW, s), payload, logical + i * SSTV_FRAME_BYTES);
            }
            /* the block covers segments seg..seg+3 = lines seg/2 and seg/2+1 */
            emitter_data_block(&em, logical, seg / 2, seg / 2 + 2);
        }
    }
    /* Parity of the last (possibly short) group, then the end meta block. */
    emitter_close_group(&em, SSTV_HEIGHT);
    meta[0] = SSTV_META_END;
    for(int i = 0; i < SSTV_ILEAVE; i++) sstv_frame_pack(sstv_hdr(SSTV_FT_META, 1), meta, logical + i * SSTV_FRAME_BYTES);
    emitter_block(&em, logical, SSTV_HEIGHT, SSTV_HEIGHT);
    return em.blocks * SSTV_ILEAVE;
}

int sstv_build_note(const char* text, const char* name, uint8_t* out, uint8_t* lines_after) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    uint8_t payload[SSTV_PAYLOAD_BYTES];

    int len = 0;
    while(text && text[len] && len < SSTV_NOTE_MAX_CHARS) len++;
    int chunks = (len + SSTV_NOTE_CHUNK - 1) / SSTV_NOTE_CHUNK; /* real text frames */
    int text_frames = ((chunks + SSTV_ILEAVE - 1) / SSTV_ILEAVE) * SSTV_ILEAVE; /* pad to a block */
    if(text_frames == 0) text_frames = SSTV_ILEAVE; /* always at least one (empty) text block */

    int data_frames = SSTV_ILEAVE + text_frames; /* START + text = the whole of group 0 */
    uint16_t total = (uint16_t)(data_frames + SSTV_OUTER_PARITY + SSTV_ILEAVE); /* + parity + END */

    pass_emitter em;
    emitter_init(&em, out, lines_after);

    /* Detect UTF-8 (any byte with the high bit set). */
    bool utf8 = false;
    for(int i = 0; i < len; i++)
        if((uint8_t)text[i] & 0x80) { utf8 = true; break; }

    /* Start meta block: [START][total | flags][content length][name] in every logical frame. */
    uint16_t word = total | SSTV_META_NOTE_FLAG | (utf8 ? SSTV_META_UTF8_FLAG : 0);
    uint8_t meta[SSTV_PAYLOAD_BYTES];
    memset(meta, 0, SSTV_PAYLOAD_BYTES);
    meta[0] = SSTV_META_START;
    meta[1] = (uint8_t)(word & 0xFF);
    meta[2] = (uint8_t)(word >> 8);
    meta[SSTV_NOTE_LEN_OFFSET] = (uint8_t)(len & 0xFF);
    meta[SSTV_NOTE_LEN_OFFSET + 1] = (uint8_t)((unsigned)len >> 8);
    if(name) {
        size_t n = strlen(name);
        if(n > SSTV_NOTE_NAME_MAX) n = SSTV_NOTE_NAME_MAX;
        memcpy(meta + SSTV_NOTE_NAME_OFFSET, name, n);
    }
    for(int i = 0; i < SSTV_ILEAVE; i++)
        sstv_frame_pack(sstv_hdr(SSTV_FT_META, 0), meta, logical + i * SSTV_FRAME_BYTES);
    emitter_data_block(&em, logical, 0, 0);

    /* Text frames: RAW, index = chunk, payload = 25 ASCII bytes (26th stays 0). */
    for(int block = 0; block < text_frames; block += SSTV_ILEAVE) {
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            int c = block + i;
            memset(payload, 0, SSTV_PAYLOAD_BYTES);
            int off = c * SSTV_NOTE_CHUNK;
            for(int k = 0; k < SSTV_NOTE_CHUNK && off + k < len; k++) payload[k] = (uint8_t)text[off + k];
            sstv_frame_pack(sstv_hdr(SSTV_FT_RAW, c), payload, logical + i * SSTV_FRAME_BYTES);
        }
        int done_before = block;              /* chunks complete before this block */
        int done_after = block + SSTV_ILEAVE; /* and after it */
        emitter_data_block(&em, logical, done_before, done_after);
    }

    /* Close the single group (its 12 parity frames), then END. */
    emitter_close_group(&em, text_frames);
    meta[0] = SSTV_META_END;
    for(int i = 0; i < SSTV_ILEAVE; i++)
        sstv_frame_pack(sstv_hdr(SSTV_FT_META, 1), meta, logical + i * SSTV_FRAME_BYTES);
    emitter_block(&em, logical, text_frames, text_frames);
    return em.blocks * SSTV_ILEAVE;
}

/* Lock-frame pattern: fixed LCG bytes (seed 0x5F7A). Random bytes are not an RS codeword;
 * tests/test_frame.c checks it never decodes, alone or deinterleaved with copies of itself
 * (which is what a receiver sees from the lock frames). */
void sstv_lock_frame(uint8_t* out) {
    uint32_t x = 0x5F7Au;
    for(int i = 0; i < SSTV_FRAME_BYTES; i++) {
        x = x * 1103515245u + 12345u;
        out[i] = (uint8_t)(x >> 16);
    }
}

/* True when `frame` is the lock-frame pattern within SSTV_LOCK_MATCH_BITS bit errors (a random
 * frame differs in ~164 of its 328 bits). */
bool sstv_is_lock_frame(const uint8_t* frame) {
    uint8_t lock[SSTV_FRAME_BYTES];
    sstv_lock_frame(lock);
    int diff = 0;
    for(int i = 0; i < SSTV_FRAME_BYTES; i++) {
        uint8_t x = (uint8_t)(lock[i] ^ frame[i]);
        while(x) { diff += x & 1; x >>= 1; }
    }
    return diff <= SSTV_LOCK_MATCH_BITS;
}
