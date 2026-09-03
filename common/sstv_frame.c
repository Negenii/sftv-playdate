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

int sstv_pass_groups(int data_frames) {
    return (data_frames + SSTV_OUTER_GROUP - 1) / SSTV_OUTER_GROUP;
}

int sstv_pass_group_size(int data_frames, int g) {
    int left = data_frames - g * SSTV_OUTER_GROUP;
    if(left <= 0) return 0;
    return left < SSTV_OUTER_GROUP ? left : SSTV_OUTER_GROUP;
}

int sstv_pass_total(int data_frames) {
    return data_frames + sstv_pass_groups(data_frames) * SSTV_OUTER_PARITY + SSTV_ILEAVE;
}

/* Walks the groups: each occupies group_size + SSTV_OUTER_PARITY physical frames. */
int sstv_pass_frame_kind(int phys, int data_frames) {
    int at = 0;
    for(int g = 0; g < sstv_pass_groups(data_frames); g++) {
        int n = sstv_pass_group_size(data_frames, g);
        if(phys < at + n) return SSTV_PASS_DATA;
        if(phys < at + n + SSTV_OUTER_PARITY) return SSTV_PASS_PARITY;
        at += n + SSTV_OUTER_PARITY;
    }
    return SSTV_PASS_END;
}

int sstv_pass_data_before(int phys, int data_frames) {
    int at = 0, data = 0;
    for(int g = 0; g < sstv_pass_groups(data_frames); g++) {
        int n = sstv_pass_group_size(data_frames, g);
        if(phys <= at + n) return data + (phys - at);
        data += n;
        at += n + SSTV_OUTER_PARITY;
        if(phys <= at) return data;
    }
    return data;
}

/* Emits one interleaved block from 4 logical frames. */
static void emit_block(const uint8_t* logical, uint8_t* out, int block_no) {
    sstv_interleave(logical, out + block_no * SSTV_ILEAVE * SSTV_FRAME_BYTES);
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

    int blocks = 0;
    /* Start meta block: [SSTV_META_START][total frames | coded flag][file name] in every
     * logical frame. Total = start + preview + data + end blocks. */
    int data_blocks = use_coded ? coded_blocks : SSTV_NUM_SEGS / SSTV_ILEAVE;
    int intro = use_coded ? SSTV_INTRO_FRAMES_CODED : SSTV_INTRO_FRAMES;
    uint16_t total = (uint16_t)((1 + intro / SSTV_ILEAVE + data_blocks + 1) * SSTV_ILEAVE);
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
    emit_block(logical, out, blocks++);
    for(int f = 0; f < SSTV_ILEAVE && lines_after; f++) lines_after[f] = 0;

    /* Tiny preview: one block of [SSTV_META_TINY][25 bytes], index = chunk. */
    uint8_t tiny[SSTV_TINY_BYTES];
    sstv_preview_make_tiny(image_bits, tiny);
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        payload[0] = SSTV_META_TINY;
        memcpy(payload + 1, tiny + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK);
        sstv_frame_pack(sstv_hdr(SSTV_FT_META, i), payload, logical + i * SSTV_FRAME_BYTES);
    }
    emit_block(logical, out, blocks);
    for(int f = 0; f < SSTV_ILEAVE && lines_after; f++) lines_after[blocks * SSTV_ILEAVE + f] = 0;
    blocks++;

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
        emit_block(logical, out, blocks);
        for(int f = 0; f < SSTV_ILEAVE && lines_after; f++) lines_after[blocks * SSTV_ILEAVE + f] = 0;
        blocks++;
    }

    if(use_coded) {
        /* Schedule: lines per coded block, one byte each, 100 bytes in one block. */
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            payload[0] = SSTV_META_SCHEDULE;
            memcpy(payload + 1, schedule + i * SSTV_PREVIEW_CHUNK, SSTV_PREVIEW_CHUNK);
            sstv_frame_pack(sstv_hdr(SSTV_FT_META, i), payload, logical + i * SSTV_FRAME_BYTES);
        }
        emit_block(logical, out, blocks);
        for(int f = 0; f < SSTV_ILEAVE && lines_after; f++) lines_after[blocks * SSTV_ILEAVE + f] = 0;
        blocks++;

        int line = 0;
        while(line < SSTV_HEIGHT) {
            int n = sstv_block_pack(image_bits, line, payload);
            if(n <= 0) break;
            for(int i = 0; i < SSTV_ILEAVE; i++) {
                sstv_frame_pack(sstv_hdr(SSTV_FT_CODED, blocks - 1), payload + i * SSTV_PAYLOAD_BYTES, logical + i * SSTV_FRAME_BYTES);
            }
            emit_block(logical, out, blocks);
            line += n;
            if(lines_after) {
                for(int f = 0; f < SSTV_ILEAVE; f++) lines_after[blocks * SSTV_ILEAVE + f] = (uint8_t)(f == SSTV_ILEAVE - 1 ? line : line - n);
            }
            blocks++;
        }
    } else {
        for(int seg = 0; seg < SSTV_NUM_SEGS; seg += SSTV_ILEAVE) {
            for(int i = 0; i < SSTV_ILEAVE; i++) {
                int s = seg + i;
                memset(payload, 0, SSTV_PAYLOAD_BYTES);
                memcpy(payload, image_bits + sstv_seg_line(s) * SSTV_LINE_BYTES + sstv_seg_byte_offset(s), SSTV_SEG_BYTES);
                sstv_frame_pack(sstv_hdr(SSTV_FT_RAW, s), payload, logical + i * SSTV_FRAME_BYTES);
            }
            emit_block(logical, out, blocks);
            if(lines_after) {
                /* the block covers segments seg..seg+3 = lines seg/2 and seg/2+1 */
                for(int f = 0; f < SSTV_ILEAVE; f++) lines_after[blocks * SSTV_ILEAVE + f] = (uint8_t)(f == SSTV_ILEAVE - 1 ? seg / 2 + 2 : seg / 2);
            }
            blocks++;
        }
    }
    /* End meta block: same payload with SSTV_META_END. */
    meta[0] = SSTV_META_END;
    for(int i = 0; i < SSTV_ILEAVE; i++) sstv_frame_pack(sstv_hdr(SSTV_FT_META, 1), meta, logical + i * SSTV_FRAME_BYTES);
    emit_block(logical, out, blocks);
    if(lines_after) {
        for(int f = 0; f < SSTV_ILEAVE; f++) lines_after[blocks * SSTV_ILEAVE + f] = (uint8_t)SSTV_HEIGHT;
    }
    blocks++;
    return blocks * SSTV_ILEAVE;
}
