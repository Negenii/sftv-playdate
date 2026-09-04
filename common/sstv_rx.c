#include "sstv_rx.h"
#include "sstv_frame.h"
#include "rs.h"
#include <string.h>

static void handle_frame(sstv_rx* rx, uint16_t hdr, const uint8_t* payload, int i, bool announce);
static void try_repair(sstv_rx* rx, int g);

void sstv_rx_set_control_cb(sstv_rx* rx, sstv_control_cb cb) {
    rx->control_cb = cb;
}

void sstv_rx_set_note_cb(sstv_rx* rx, sstv_note_cb cb) {
    rx->note_cb = cb;
}

void sstv_rx_init(sstv_rx* rx, sstv_pixel_cb pixel_cb, sstv_meta_cb meta_cb, sstv_preview_cb preview_cb, void* ctx) {
    memset(rx, 0, sizeof *rx);
    rx->pixel_cb = pixel_cb;
    rx->meta_cb = meta_cb;
    rx->preview_cb = preview_cb;
    rx->control_cb = NULL;
    rx->note_cb = NULL;
    rx->note = false;
    rx->ctx = ctx;
    sstv_rx_start_pass(rx);
}

void sstv_rx_reset_counters(sstv_rx* rx) {
    rx->frames_in = 0;
    rx->blocks_ok = 0;
    rx->frames_ok = 0;
    rx->frames_bad = 0;
    rx->rs_corrected = 0;
    rx->frames_repaired = 0;
    rx->groups_repaired = 0;
    rx->repairs_failed = 0;
}

void sstv_rx_start_pass(sstv_rx* rx) {
    memset(rx->have, 0, sizeof rx->have);
    memset(rx->have_parity, 0, sizeof rx->have_parity);
    memset(rx->group_done, 0, sizeof rx->group_done);
    rx->data_frames = 0;
}

/* Byte confidence = weakest of its bits. */
static void byte_confidence(const float* soft, float* conf) {
    for(int j = 0; j < SSTV_FRAME_BYTES; j++) {
        float c = 1.0f;
        for(int b = 0; b < 8; b++) {
            float m = soft[j * 8 + b];
            if(m < 0) m = -m;
            if(m < c) c = m;
        }
        conf[j] = c;
    }
}

static void coded_line(void* ctx, uint16_t line, const uint8_t* pixels) {
    sstv_rx* rx = ctx;
    if(rx->pixel_cb) rx->pixel_cb(rx->ctx, line, 0, SSTV_LINE_BYTES, pixels);
}

/* Inverse of sstv_pass_total: data positions of a pass announcing `total` frames, 0 if the
 * total fits no layout (e.g. a revision 6 transmitter). */
static int data_frames_from_total(int total, int coded, int note) {
    if(note) {
        int d = total - SSTV_OUTER_PARITY - SSTV_ILEAVE; /* single group: intro = data_frames */
        return d > 0 ? d : 0;
    }
    int intro = sstv_pass_intro(coded);
    for(int d = 1; d <= SSTV_MAX_DATA_FRAMES; d++) {
        if(sstv_pass_total(d, intro) == total) return d;
    }
    return 0;
}

/* Group-0 size / intro for the current pass (a note is one group = all its data frames). */
static int rx_intro(const sstv_rx* rx) {
    return rx->note ? rx->data_frames : sstv_pass_intro(rx->coded);
}

/* Header of the frame at a data position (raw or coded pass). */
static uint16_t header_for_position(const sstv_rx* rx, int pos) {
    if(pos < SSTV_ILEAVE) return sstv_hdr(SSTV_FT_META, 0);
    if(rx->note) return sstv_hdr(SSTV_FT_RAW, pos - SSTV_ILEAVE);
    if(pos < SSTV_ILEAVE + SSTV_TINY_CHUNKS) return sstv_hdr(SSTV_FT_META, pos - SSTV_ILEAVE);
    if(pos < SSTV_ILEAVE + SSTV_INTRO_FRAMES) return sstv_hdr(SSTV_FT_META, pos - SSTV_ILEAVE - SSTV_TINY_CHUNKS);
    if(rx->coded) {
        if(pos < SSTV_ILEAVE + SSTV_INTRO_FRAMES_CODED) return sstv_hdr(SSTV_FT_META, pos - SSTV_ILEAVE - SSTV_INTRO_FRAMES);
        return sstv_hdr(SSTV_FT_CODED, pos / SSTV_ILEAVE - 1);
    }
    return sstv_hdr(SSTV_FT_RAW, pos - SSTV_ILEAVE - SSTV_INTRO_FRAMES);
}

/* Unpacks coded block `index` from the stored payloads of its four positions, up to the
 * first missing one. */
static void coded_block_unpack(sstv_rx* rx, int index) {
    int base = (index + 1) * SSTV_ILEAVE;
    if(base < 0 || base + SSTV_ILEAVE > SSTV_MAX_DATA_FRAMES) return;
    int valid = 0;
    while(valid < SSTV_ILEAVE && rx->have[base + valid]) valid++;
    if(valid == 0) return;
    sstv_block_unpack(&rx->data[base][0], valid * SSTV_PAYLOAD_BYTES, coded_line, rx);
}

static void store(sstv_rx* rx, int pos, const uint8_t* payload) {
    if(pos < 0 || pos >= SSTV_MAX_DATA_FRAMES) return;
    memcpy(rx->data[pos], payload, SSTV_PAYLOAD_BYTES);
    rx->have[pos] = 1;
    /* a late data frame may complete a group whose parity already arrived */
    int g = sstv_pass_group_of(pos, rx_intro(rx));
    if(g < SSTV_OUTER_GROUPS_MAX && !rx->group_done[g]) {
        for(int j = 0; j < SSTV_OUTER_PARITY; j++) {
            if(rx->have_parity[g][j]) {
                try_repair(rx, g);
                break;
            }
        }
    }
}

/* Dispatches one decoded logical frame (from the demodulator or from a repair) and stores it
 * by position. i = the frame's slot in its block (START positions 0..3 share index 0).
 * announce: fire meta_cb and open the pass on START (false for repeated START frames and
 * repaired frames). Coded blocks are unpacked by the caller. */
static void handle_frame(sstv_rx* rx, uint16_t hdr, const uint8_t* payload, int i, bool announce) {
    int type = sstv_hdr_type(hdr), index = sstv_hdr_index(hdr);
    if(type == SSTV_FT_PARITY) {
        int g = index / SSTV_OUTER_PARITY, j = index % SSTV_OUTER_PARITY;
        if(g >= SSTV_OUTER_GROUPS_MAX) return;
        memcpy(rx->parity[g][j], payload, SSTV_PAYLOAD_BYTES);
        rx->have_parity[g][j] = 1;
        try_repair(rx, g);
        return;
    }
    if(type == SSTV_FT_META) {
        int sub = payload[0];
        if(sub == SSTV_META_PREVIEW || sub == SSTV_META_TINY || sub == SSTV_META_SCHEDULE) {
            int layer = sub == SSTV_META_TINY ? 0 : sub == SSTV_META_PREVIEW ? 1 : 2;
            static const int totals[3] = {SSTV_TINY_BYTES, SSTV_PREVIEW_BYTES, SSTV_MAX_CODED_BLOCKS};
            static const int chunks[3] = {SSTV_TINY_CHUNKS, SSTV_PREVIEW_CHUNKS, SSTV_SCHEDULE_CHUNKS};
            if(sub == SSTV_META_SCHEDULE) rx->coded = true; /* schedule exists only in coded passes */
            if(index >= chunks[layer]) return;
            int n = totals[layer] - index * SSTV_PREVIEW_CHUNK;
            if(n > SSTV_PREVIEW_CHUNK) n = SSTV_PREVIEW_CHUNK;
            if(rx->preview_cb) rx->preview_cb(rx->ctx, layer, index, n, payload + 1);
        } else if(sub == SSTV_META_START || sub == SSTV_META_END) {
            uint16_t word = (uint16_t)(payload[1] | (payload[2] << 8));
            bool coded = (word & SSTV_META_CODED_FLAG) != 0;
            bool note = (word & SSTV_META_NOTE_FLAG) != 0;
            int total = word & ~(SSTV_META_CODED_FLAG | SSTV_META_NOTE_FLAG);
            rx->note = note;
            if(sub == SSTV_META_START && announce) {
                if(!rx->pass_open) {
                    sstv_rx_start_pass(rx);
                    rx->pass_open = true;
                }
                rx->coded = coded;
            }
            if(sub == SSTV_META_END) rx->pass_open = false;
            rx->data_frames = data_frames_from_total(total, coded, note);
            if(sub == SSTV_META_END) {
                /* last chance: groups whose size was unknown (START lost) or whose parity
                   arrived before their last data frame */
                for(int g = 0; g < SSTV_OUTER_GROUPS_MAX; g++) try_repair(rx, g);
            }
            if(announce && rx->meta_cb) {
                char name[SSTV_PAYLOAD_BYTES];
                memcpy(name, payload + SSTV_META_NAME_OFFSET, SSTV_NAME_MAX);
                name[SSTV_NAME_MAX] = 0;
                rx->meta_cb(rx->ctx, name, sub == SSTV_META_END, total, coded);
            }
        } else if(sub == SSTV_META_PAUSE || sub == SSTV_META_RESUME) {
            if(rx->control_cb) {
                uint32_t arg = (uint32_t)(payload[1] | (payload[2] << 8));
                rx->control_cb(rx->ctx, sub, arg);
            }
            return; /* control frames carry no data position */
        } else {
            return;
        }
        store(rx, sstv_pass_meta_position(sub, index, i), payload);
    } else if(type == SSTV_FT_RAW) {
        if(rx->note) {
            if(rx->note_cb) rx->note_cb(rx->ctx, index, SSTV_NOTE_CHUNK, payload);
            store(rx, SSTV_ILEAVE + index, payload);
        } else {
            rx->coded = false;
            if(rx->pixel_cb) {
                rx->pixel_cb(rx->ctx, (uint16_t)sstv_seg_line(index), sstv_seg_byte_offset(index), SSTV_SEG_BYTES, payload);
            }
            store(rx, sstv_pass_position(hdr, i), payload);
        }
    } else if(type == SSTV_FT_CODED) {
        rx->coded = true;
        store(rx, sstv_pass_position(hdr, i), payload);
    }
}

/* Erasure-decodes group g column by column once its parity is in and at most
 * SSTV_OUTER_PARITY frames (data + parity) are missing. Recovered frames get their header
 * from their position and go through handle_frame; touched coded blocks are unpacked again. */
static void try_repair(sstv_rx* rx, int g) {
    if(g >= SSTV_OUTER_GROUPS_MAX || rx->group_done[g]) return;
    /* Without START the pass length is unknown: assume a full group. A short last group then
     * looks like it has too many missing frames, so nothing happens until END announces the
     * length and the final sweep runs. */
    int intro = rx_intro(rx);
    /* Without START/END the pass length is unknown: assume full groups (intro, then 108). A
     * short last group then looks over-erased and waits for the END sweep. */
    int n = rx->data_frames > 0 ? sstv_pass_group_size(rx->data_frames, intro, g) : (g == 0 ? intro : SSTV_OUTER_GROUP);
    if(n <= 0) return;
    int base = sstv_pass_group_base(intro, g);
    int erasures[SSTV_OUTER_PARITY];
    int ne = 0, missing_data = 0;
    for(int p = 0; p < n; p++) {
        if(rx->have[base + p]) continue;
        if(ne < SSTV_OUTER_PARITY) erasures[ne] = p;
        ne++;
        missing_data++;
    }
    for(int j = 0; j < SSTV_OUTER_PARITY; j++) {
        if(rx->have_parity[g][j]) continue;
        if(ne < SSTV_OUTER_PARITY) erasures[ne] = n + j;
        ne++;
    }
    if(missing_data == 0) {
        rx->group_done[g] = 1;
        return;
    }
    if(ne > SSTV_OUTER_PARITY) return; /* maybe later, when more of the group arrives */
    uint8_t cw[SSTV_OUTER_GROUP + SSTV_OUTER_PARITY];
    for(int k = 0; k < SSTV_PAYLOAD_BYTES; k++) {
        for(int p = 0; p < n; p++) cw[p] = rx->have[base + p] ? rx->data[base + p][k] : 0;
        for(int j = 0; j < SSTV_OUTER_PARITY; j++) cw[n + j] = rx->have_parity[g][j] ? rx->parity[g][j][k] : 0;
        if(rs_decode_erasures(cw, n + SSTV_OUTER_PARITY, erasures, ne) < 0) {
            rx->repairs_failed++;
            return;
        }
        for(int p = 0; p < n; p++) {
            if(!rx->have[base + p]) rx->data[base + p][k] = cw[p];
        }
    }
    rx->group_done[g] = 1;
    rx->groups_repaired++;
    uint8_t touched[SSTV_OUTER_GROUP / SSTV_ILEAVE + 2];
    memset(touched, 0, sizeof touched);
    for(int p = 0; p < n; p++) {
        int pos = base + p;
        if(rx->have[pos]) continue;
        rx->have[pos] = 1;
        rx->frames_repaired++;
        uint16_t hdr = header_for_position(rx, pos);
        if(sstv_hdr_type(hdr) == SSTV_FT_CODED) touched[p / SSTV_ILEAVE] = 1;
        /* handle_frame stores again (harmless) and delivers pixels / previews */
        handle_frame(rx, hdr, rx->data[pos], pos % SSTV_ILEAVE, false);
    }
    for(int b = 0; b < (int)sizeof touched; b++) {
        if(!touched[b]) continue;
        int pos = base + b * SSTV_ILEAVE;
        coded_block_unpack(rx, pos / SSTV_ILEAVE - 1);
    }
}

/* Tries to decode the window as one block. Returns the number of good logical frames. */
static int try_block(sstv_rx* rx) {
    uint8_t logical[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    float lconf[SSTV_ILEAVE * SSTV_FRAME_BYTES];
    sstv_deinterleave(&rx->phys[0][0], logical);
    sstv_deinterleave_conf(&rx->conf[0][0], lconf);

    uint16_t hdr[SSTV_ILEAVE];
    uint8_t payload[SSTV_BLOCK_PAYLOAD];
    bool ok[SSTV_ILEAVE];
    int corr[SSTV_ILEAVE];
    int good = 0;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        ok[i] = sstv_frame_unpack_soft(
            logical + i * SSTV_FRAME_BYTES, lconf + i * SSTV_FRAME_BYTES, &hdr[i],
            payload + i * SSTV_PAYLOAD_BYTES, &corr[i]);
        if(ok[i]) good++;
    }
    if(good == 0) return 0;
    /* All good frames must agree on the type (and, for coded blocks, on the block index). */
    int type = -1, index = -1;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(!ok[i]) continue;
        int t = sstv_hdr_type(hdr[i]), x = sstv_hdr_index(hdr[i]);
        if(type >= 0 && t != type) return 0;
        if(t == SSTV_FT_CODED && index >= 0 && x != index) return 0;
        if((t == SSTV_FT_RAW || t == SSTV_FT_PARITY) && index >= 0 && x != index + i) return 0;
        type = t;
        index = (t == SSTV_FT_RAW || t == SSTV_FT_PARITY) ? x - i : x;
    }
    rx->blocks_ok++;
    rx->last_type = type;
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(ok[i]) {
            rx->frames_ok++;
            if(corr[i] > 0) rx->rs_corrected += corr[i];
        } else {
            rx->frames_bad++;
        }
    }
    bool announced = false; /* START / END: the same payload in every frame, report once */
    for(int i = 0; i < SSTV_ILEAVE; i++) {
        if(!ok[i]) continue;
        const uint8_t* p = payload + i * SSTV_PAYLOAD_BYTES;
        bool meta_named = type == SSTV_FT_META && (p[0] == SSTV_META_START || p[0] == SSTV_META_END);
        handle_frame(rx, hdr[i], p, i, !meta_named || !announced);
        if(meta_named) announced = true;
    }
    /* Provisional glitch (raw mode): a half-line whose frame failed to decode but whose segment
     * is known from the block's good frames is drawn now with its best-effort bytes; a later
     * outer-code repair overwrites it with the correct pixels. Not stored, so the repair runs. */
    if(type == SSTV_FT_RAW && index >= 0) {
        for(int i = 0; i < SSTV_ILEAVE; i++) {
            if(ok[i]) continue;
            int seg = index + i;
            if(seg >= 0 && seg < SSTV_NUM_SEGS && rx->pixel_cb)
                rx->pixel_cb(rx->ctx, (uint16_t)sstv_seg_line(seg), sstv_seg_byte_offset(seg),
                             SSTV_SEG_BYTES, payload + i * SSTV_PAYLOAD_BYTES);
        }
    }
    if(type == SSTV_FT_CODED) coded_block_unpack(rx, index);
    return good;
}

void sstv_rx_phys(sstv_rx* rx, const uint8_t* frame, const float* soft) {
    rx->frames_in++;
    if(rx->count == SSTV_ILEAVE) {
        memmove(rx->phys[0], rx->phys[1], sizeof(rx->phys) - sizeof(rx->phys[0]));
        memmove(rx->conf[0], rx->conf[1], sizeof(rx->conf) - sizeof(rx->conf[0]));
        rx->count--;
    }
    memcpy(rx->phys[rx->count], frame, SSTV_FRAME_BYTES);
    byte_confidence(soft, rx->conf[rx->count]);
    rx->count++;
    if(rx->count < SSTV_ILEAVE) return;
    if(try_block(rx) > 0) rx->count = 0; /* block consumed; next one starts fresh */
}
