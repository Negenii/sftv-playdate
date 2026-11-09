#pragma once

/* Image geometry: Playdate screen, 1 bit per pixel, PBM P4 packing (bit 1 = black). */
#define SSTV_WIDTH 400
#define SSTV_HEIGHT 240
#define SSTV_LINE_BYTES 50 /* ceil(400 / 8) */
#define SSTV_IMAGE_BYTES (SSTV_LINE_BYTES * SSTV_HEIGHT) /* 12000 */

/* Frames, segments and blocks. Every logical frame is [header, 2 bytes][SSTV_PAYLOAD_BYTES]
 * [CRC-8] + SSTV_PARITY_BYTES Reed-Solomon parity (corrects PARITY/2 unknown or PARITY erased
 * bytes). SSTV_ILEAVE logical frames are byte-interleaved into SSTV_ILEAVE physical frames so
 * a noise burst hitting one physical frame is spread thinly. The header's top two bits give
 * the frame type, the rest an index:
 *   SSTV_FT_RAW   payload = one half line (SSTV_SEG_BYTES pixels), index = segment 0..479.
 *                 Frames are independent: a lost frame costs half a line.
 *   SSTV_FT_CODED payload = a quarter of a 104-byte block of losslessly coded consecutive
 *                 lines ([count][first line][records]...), index = block. Used when the image
 *                 compresses by at least SSTV_CODED_MIN_GAIN (line art, text), where a block
 *                 carries 4-10 lines and the transfer is that much shorter.
 *   SSTV_FT_META  payload = [subtype][data]. Subtypes SSTV_META_START (opens a transmission)
 *                 and SSTV_META_END (closes it) carry [total physical frames, 16-bit LE, bit 15
 *                 set when the picture goes as CODED blocks][file name, SSTV_NAME_MAX bytes],
 *                 repeated in all four logical frames of the block. The frame count lets the
 *                 receiver time the whole pass (every physical frame is SSTV_PHYS_US) and
 *                 animate independently of lost frames. Then, progressive-JPEG style, one
 *                 block of SSTV_META_TINY frames carries a 40x20 dithered preview (0.67 s) and
 *                 SSTV_META_PREVIEW frames carry (see below). In coded passes one block of
 *                 SSTV_META_SCHEDULE frames follows with the number of lines in each coded
 *                 block, so the receiver can animate at the real line rate.
 *                 SSTV_META_PREVIEW frames carry
 *                 SSTV_PREVIEW_CHUNK bytes of an 80x48 dithered preview of the image, header
 *                 index = chunk number. They follow START so the receiver can show what is
 *                 coming (and use it as a blurred placeholder) 3.4 s into the transfer. */
#define SSTV_PAYLOAD_BYTES 26 /* 4 x 26 = 104: two raw lines (1 + 1 + 2 x 51) fit in a block */
#define SSTV_SEG_BYTES 25 /* half a line */
#define SSTV_SEGS_PER_LINE 2
#define SSTV_NUM_SEGS (SSTV_HEIGHT * SSTV_SEGS_PER_LINE) /* 480 */
#define SSTV_META_NAME_OFFSET 3 /* [subtype][frames lo][frames hi][name...] */
#define SSTV_NAME_MAX (SSTV_PAYLOAD_BYTES - SSTV_META_NAME_OFFSET) /* 23 characters */
#define SSTV_META_CODED_FLAG 0x8000
#define SSTV_META_NOTE_FLAG 0x4000 /* START/END total word: this pass carries a text note (never coded, so bit 15 stays 0) */
#define SSTV_META_UTF8_FLAG 0x2000 /* note charset: set = UTF-8 bytes, clear = plain ASCII */
/* Animation (revision 12): NOTE and CODED together (a note is never coded). The pass is laid out
 * like a note: START [total | flags][content length][name], the byte stream of sstv_anim.h in RAW
 * frames of SSTV_NOTE_CHUNK bytes, outer-code groups, END. */
#define SSTV_META_ANIM_FLAGS (SSTV_META_NOTE_FLAG | SSTV_META_CODED_FLAG)
/* Notes (revision 11): up to SSTV_NOTE_MAX_BYTES on the wire, laid out like a picture: group 0 is
 * START + the first text frames up to SSTV_OUTER_GROUP, then groups of SSTV_OUTER_GROUP. A note of
 * up to 104 text frames (2600 bytes) keeps the old single-group layout exactly.
 * SSTV_NOTE_MAX_CHARS is what the C builder (the Flipper) sends: it fits the picture buffer
 * (SSTV_MAX_FRAMES). Receivers take the full SSTV_NOTE_MAX_BYTES. */
#define SSTV_NOTE_MAX_BYTES 65535 /* the 16-bit content length */
#define SSTV_NOTE_MAX_CHARS 12000 /* 480 text frames -> 548 physical, within SSTV_MAX_FRAMES */
#define SSTV_NOTE_CHUNK SSTV_SEG_BYTES /* 25 ASCII bytes per RAW text frame */
#define SSTV_NOTE_MAX_FRAMES ((((SSTV_NOTE_MAX_CHARS + SSTV_NOTE_CHUNK - 1) / SSTV_NOTE_CHUNK) + SSTV_ILEAVE - 1) / SSTV_ILEAVE * SSTV_ILEAVE) /* 40 */
/* Note START/END meta: [subtype][total lo][total hi][len lo][len hi][name...]. The content
 * length (bytes) is announced up front like HTTP Content-Length, so both ends agree on the
 * exact frame count and a short note sends only as many blocks as it needs. */
#define SSTV_NOTE_LEN_OFFSET 3
#define SSTV_NOTE_NAME_OFFSET 5
#define SSTV_NOTE_NAME_MAX (SSTV_PAYLOAD_BYTES - SSTV_NOTE_NAME_OFFSET) /* 21 */
#define SSTV_META_START 0
#define SSTV_META_END 1
#define SSTV_META_PREVIEW 2
#define SSTV_META_TINY 3 /* first, coarser preview layer: 40x20 in one block */
#define SSTV_TINY_W 40 /* SSTV_WIDTH / 10 */
#define SSTV_TINY_H 20 /* SSTV_HEIGHT / 12 */
#define SSTV_TINY_LINE_BYTES 5
#define SSTV_TINY_BYTES (SSTV_TINY_LINE_BYTES * SSTV_TINY_H) /* 100 = exactly one block */
#define SSTV_TINY_CHUNKS 4
#define SSTV_PREVIEW_W 80 /* SSTV_WIDTH / 5 */
#define SSTV_PREVIEW_H 48 /* SSTV_HEIGHT / 5 */
#define SSTV_PREVIEW_LINE_BYTES 10
#define SSTV_PREVIEW_BYTES (SSTV_PREVIEW_LINE_BYTES * SSTV_PREVIEW_H) /* 480 */
#define SSTV_PREVIEW_CHUNK (SSTV_PAYLOAD_BYTES - 1) /* 25 */
#define SSTV_PREVIEW_CHUNKS ((SSTV_PREVIEW_BYTES + SSTV_PREVIEW_CHUNK - 1) / SSTV_PREVIEW_CHUNK) /* 20 = 5 blocks */
#define SSTV_META_SCHEDULE 4 /* coded passes only: lines per block, one byte each, one block */
#define SSTV_META_PAUSE 5 /* transmitter paused: receiver waits, picture frozen (payload[1..2] = paused-at frame) */
#define SSTV_META_RESUME 6 /* transmitter resuming: payload[1..2] = next physical frame index, so the receiver realigns its animation clock */
#define SSTV_SCHEDULE_CHUNKS 4
#define SSTV_MAX_CODED_BLOCKS (SSTV_SCHEDULE_CHUNKS * SSTV_PREVIEW_CHUNK) /* 100: the schedule must fit one block */
#define SSTV_INTRO_FRAMES (SSTV_TINY_CHUNKS + SSTV_PREVIEW_CHUNKS) /* preview frames between START and data (raw) */
#define SSTV_INTRO_FRAMES_CODED (SSTV_INTRO_FRAMES + SSTV_SCHEDULE_CHUNKS) /* coded passes add the schedule block */
#define SSTV_DATA_BYTES (2 + SSTV_PAYLOAD_BYTES + 1) /* 29 */
/* Revision 14: 16 parity bytes (was 12). A browser recording (2026-11-07) failed at ~1.5 % bit
 * errors, where a 41-byte frame averages ~4.5 bad bytes against the 6 that 12 parity correct;
 * 16 correct 8 (and the outer code, which shares the RS code, now rebuilds 16 lost frames per
 * group). Costs ~12 % on the air. */
#ifndef SSTV_PARITY_BYTES
#define SSTV_PARITY_BYTES 16
#endif
#define SSTV_FRAME_BYTES (SSTV_DATA_BYTES + SSTV_PARITY_BYTES) /* 45 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 360 */
#ifndef SSTV_ILEAVE
#define SSTV_ILEAVE 4
#endif
#define SSTV_BLOCK_PAYLOAD (SSTV_ILEAVE * SSTV_PAYLOAD_BYTES) /* 104 */
#define SSTV_FT_RAW 0
#define SSTV_FT_CODED 1
#define SSTV_FT_META 2
#define SSTV_FT_PARITY 3 /* outer code: byte-column RS parity over a group of logical frames */
#define SSTV_FT_SHIFT 14
#define SSTV_INDEX_MASK 0x3FFF
#define SSTV_MAX_BLOCKS (SSTV_HEIGHT / 2) /* coded worst case: raw lines, 2 per block */
/* Outer code (revision 7): every SSTV_OUTER_GROUP data frames (all logical frames except END,
 * in transmission order) are followed by SSTV_OUTER_PARITY parity frames; any that many lost
 * frames of the group are rebuilt by the receiver. END comes after the last parity block. */
#define SSTV_OUTER_GROUP 108 /* 27 blocks, ~73 s */
#define SSTV_OUTER_PARITY SSTV_PARITY_BYTES /* 12 = 3 blocks, +11 % */
#define SSTV_MAX_DATA_FRAMES (SSTV_ILEAVE + SSTV_INTRO_FRAMES_CODED + SSTV_NUM_SEGS) /* 512 */
#define SSTV_OUTER_GROUPS_MAX (1 + (SSTV_MAX_DATA_FRAMES + SSTV_OUTER_GROUP - 1) / SSTV_OUTER_GROUP) /* intro group + data groups */
#define SSTV_MAX_FRAMES (SSTV_MAX_DATA_FRAMES + SSTV_OUTER_GROUPS_MAX * SSTV_OUTER_PARITY + SSTV_ILEAVE)
/* Receiver capacity: the longest pass is a 64 KB note. */
#define SSTV_NOTE_MAX_TEXT_FRAMES ((((SSTV_NOTE_MAX_BYTES + SSTV_NOTE_CHUNK - 1) / SSTV_NOTE_CHUNK) + SSTV_ILEAVE - 1) / SSTV_ILEAVE * SSTV_ILEAVE) /* 2624 */
#define SSTV_RX_MAX_DATA_FRAMES (SSTV_ILEAVE + SSTV_NOTE_MAX_TEXT_FRAMES) /* 2628 */
#define SSTV_RX_GROUPS_MAX (1 + (SSTV_RX_MAX_DATA_FRAMES + SSTV_OUTER_GROUP - 1) / SSTV_OUTER_GROUP) /* 26 */
#define SSTV_NOTE_INTRO(data_frames) ((data_frames) < SSTV_OUTER_GROUP ? (data_frames) : SSTV_OUTER_GROUP) /* group 0 of a note */
#define SSTV_CODED_MIN_GAIN 0.85f /* use coding only when frames <= 85% of raw */

/* Modulation: 4-FSK, 2 bits per 4 ms symbol, square-wave tones.
 *
 * Why 4 ms and four tones: on a Flipper buzzer -> Playdate microphone path the buzzer rings
 * at its ~3 kHz resonance at every frequency change and room/case tails at some frequencies
 * last several ms. With 2 ms symbols the previous symbol dominated the decision window.
 * A 4 ms symbol with the decision taken over its last 2 ms gives tails 2 ms to fall by
 * 20 dB; two bits per symbol keep the line time close to the 2-FSK design (1.18 s).
 *
 * Why these tones: all are multiples of 500 Hz, so they are orthogonal over the 2 ms
 * detector window. None is 2x or 3x another (the buzzer is nonlinear: 2000/4000 put a
 * second harmonic straight into the other detector). A Playdate recording showed the tone
 * level swinging with device position by 6 dB at 3.5 kHz but 20 dB at 6 kHz (shorter
 * wavelength, deeper interference notches; high tones are also more directional), so the
 * set sits as low as the buzzer allows: 2000-2250 Hz ring for 15 ms and are out, 2500 decays
 * in 2.4 ms, 3000 Hz is the buzzer resonance (loud, decays in 1.2 ms, harmless behind the
 * 2 ms guard). Symbol value v (0..3) is SSTV_TONE_HZ[v]. */
#define SSTV_NUM_TONES 4
#define SSTV_BITS_PER_SYMBOL 2
/* Symbol 0 is silence (tone 0 Hz): white image areas go quiet and only drawn pixels sound.
 * The receiver declares silence when no tone reaches SSTV_SILENCE_LEVEL of its tracked peak.
 * 2500 Hz was dropped as a tone: at the Playdate its tail lasted 2-4 ms, so after a 2500
 * symbol the following silence still carried -10 dB of it and the present/absent contrast
 * shrank to ~5 dB (3000 and 3500 have 12-20 dB). */
#define SSTV_TONE0_HZ 0
#define SSTV_TONE1_HZ 3000
#define SSTV_TONE2_HZ 3500
#define SSTV_TONE3_HZ 4000
#define SSTV_SILENCE_LEVEL 0.3f /* a tone is present when >= this fraction of its tracked peak */
#define SSTV_TONES_INIT {SSTV_TONE0_HZ, SSTV_TONE1_HZ, SSTV_TONE2_HZ, SSTV_TONE3_HZ}
/* Transmit pre-emphasis: PWM duty per tone, percent. The fundamental of a square wave scales
 * with sin(pi * duty), so a narrow pulse attenuates a tone without touching the others:
 * 12 % = -10 dB, 30 % = -4 dB. The buzzer's 3 kHz resonance arrived 10-27 dB louder than
 * the other tones at a Playdate microphone; this evens the set out (and quietens the whine). */
#define SSTV_TONE_DUTY_PCT_INIT {0, 12, 30, 50}
#define SSTV_SYNC_DUTY_PCT 50
#ifndef SSTV_FSYNC_HZ
#define SSTV_FSYNC_HZ 5500 /* frame sync: 500 Hz grid, no 2x/3x relation, 1.3 ms decay (5000 Hz rang for 5 ms and blurred the sync edge) */
#endif

/* Near-ultrasound band (same protocol and timing, inaudible to adults). Measured 2026-11-06 with
 * the Flipper HF sweep at a Playdate mic, 4-5 cm (recordings/2026-11-06-hf/pd_test9.wav): vs the
 * 3 kHz reference, 18.0 kHz -31 dB / SNR 37, 18.5 -28 / 42, 19.0 -33 / 33, 21.0 -35 / 42 dB;
 * 14 and 15.5 kHz are notches (SNR 7-11). The room has almost no noise up there, so the SNR
 * matches the audible band's ~40 dB. The user hears the sweep up to ~13 kHz. All 50 % duty. */
#define SSTV_BAND_AUDIBLE 0
#define SSTV_BAND_HF 1
/* HF symbols are 3 ms (4/3 the audible band's rate). Every symbol must hold a whole number of
 * tone cycles on the Flipper timer, so the tones sit on 18/19/21 kHz (54/57/63 cycles; 18.5 kHz
 * would be 55.5) and the sync, 16 ms long as in the audible band, on 20.5 kHz (328 cycles). The
 * 2 ms detector window leaves ~1 ms after each transition for the tail to die.
 * 2 ms symbols (1 ms window, sync 20 kHz) were tried on hardware 2026-11-06 and failed: best
 * tuning decoded 28/240 lines (recordings/2026-11-06-hf/pd_hf2ms_test10.wav); decisions got
 * better the earlier in the symbol they were taken (transition tails fill the end of a 2 ms
 * symbol) and 38 % of the 20 kHz syncs were missed. 3 ms decoded without errors. */
#define SSTV_HF_DUTY_PCT 50
#define SSTV_HF_SYMBOL_US 3000
#define SSTV_HF_WINDOW_US 2000
/* 8-FSK (revision 10): 7 tones + silence = 3 bits per symbol, a frame's 328 bits in 110 symbols
 * (2 pad bits), 346 ms per frame instead of 508 (x1.47). 500 Hz grid (orthogonal over the 2 ms
 * window), 17.5 kHz skipped (SNR 24 in the sweep), sync on 21 kHz (the strongest, SNR 42). Not
 * every tone fits a whole number of cycles into 3 ms (18.5 kHz = 55.5): the Flipper alternates
 * the cycle count so the average symbol is exactly 3 ms (+-27 us jitter). */
#define SSTV_MAX_TONES 8
#define SSTV_HF_NUM_TONES 8
#define SSTV_HF_BITS_PER_SYMBOL 3
/* No silence symbol in the HF band: on hardware (recordings/2026-11-06-hf/pd_8fsk_lazy_test12.wav)
 * 11k verified symbols had zero tone-for-tone confusions; ~all errors were tone <-> silence
 * (18 kHz read as silence 3.4 %), the same echo/fade problem as the audible band. Inaudible
 * anyway, so the 8th value is a tone (17.5 kHz) instead of silence. */
#define SSTV_HF_TONES_INIT {17000, 17500, 18000, 18500, 19000, 19500, 20000, 20500}
#define SSTV_HF_HAS_SILENCE 0
#define SSTV_HF_FSYNC_HZ 21000 /* revision 12 and before; the HF band now syncs on a pattern */
/* Pattern sync (revision 13): the HF frame starts with SSTV_HF_SYNC_SYMBOLS ordinary data-tone
 * symbols in a fixed order instead of a 21 kHz burst. A laptop microphone heard 21 kHz 18 dB under
 * the data tones and never locked (recording 2026-11-07): a sync made of the data tones is heard
 * by anything that hears the data. Every tone once, neighbours >= 1.5 kHz apart; the receiver
 * correlates each tone's share of the energy (each tone normalised by its own tracked peak, so an
 * uneven microphone response does not matter) against the pattern. 24 ms, frames 354 ms. */
#define SSTV_HF_SYNC_SYMBOLS 8
#define SSTV_HF_SYNC_PATTERN_INIT {0, 4, 7, 3, 6, 2, 5, 1}
/* Correlation needed to accept the pattern: mean share of the expected tone over the 8 symbols.
 * Random data scores ~0.15 on average. */
#ifndef SSTV_SYNC_SCORE_MIN
#define SSTV_SYNC_SCORE_MIN 0.45f
#endif
/* HF fundamentals arrive at ~25-55 LSB at the Playdate (3 kHz ~1400), noise ~1 LSB: the audible
 * band's 40 LSB floor would hide them. */
#define SSTV_HF_NOISE_FLOOR_LSB 5.0f

/* Timing, microseconds. Every duration is a whole number of cycles of its tone. */
#ifndef SSTV_SYMBOL_US
#define SSTV_SYMBOL_US 4000
#endif
#ifndef SSTV_SYNC_US
#define SSTV_SYNC_US 16000
#endif
/* Start preamble (revision 9): a tone ladder, then lock frames, then the pass.
 * Ladder: each data tone 1..3 held SSTV_LADDER_TONE_SYMBOLS, then SSTV_LADDER_GAP_SYMBOLS of
 * silence, so the receiver's per-tone gains settle on each tone alone (0.9 s).
 * Lock frames: SSTV_LOCK_FRAMES ordinary physical frames (sync + 164 symbols) carrying a fixed
 * pseudo-random pattern that never passes the RS check (sstv_lock_frame). The receiver locks
 * its symbol clock and measures the sync-vs-data margin on them, so the pass's first frame
 * (START, with the file name) arrives already in rhythm (2 s). Old receivers drop them as bad. */
#define SSTV_LADDER_TONE_SYMBOLS 50 /* 200 ms per tone */
#define SSTV_LADDER_GAP_SYMBOLS 25 /* 100 ms silence after each */
#define SSTV_LOCK_FRAMES 3
/* Receivers recognise a lock frame by its pattern (<= this many of 328 bits wrong) and keep it
 * out of the deinterleave window: mixed with the first START frames, its bytes could be
 * "repaired" by erasure decoding into a false block that misaligns the window and loses START. */
#define SSTV_LOCK_MATCH_BITS 66 /* ~18 % of SSTV_FRAME_BITS */

/* Receiver tuning. */
#define SSTV_WINDOW_US 2000 /* detector window: tone resolution 500 Hz */
#define SSTV_SYNC_WINDOW_MULT 4 /* sync detector window = 4 data windows (+6 dB on sync) */
#ifndef SSTV_SAMPLE_POINT
#define SSTV_SAMPLE_POINT 0.95f /* fraction of the symbol at which the window ends */
#endif
#ifndef SSTV_EDGE_ADJUST_US
#define SSTV_EDGE_ADJUST_US -300 /* echoes delay the sync->data crossover; pull the edge back */
#endif
#ifndef SSTV_NOISE_FLOOR_LSB
#define SSTV_NOISE_FLOOR_LSB 40.0f /* square-wave amplitude below which detectors are ignored */
#endif
#define SSTV_GAIN_TAU_S 0.5f /* per-tone peak tracker decay time constant */
#define SSTV_SYNC_GAIN_TAU_S 3.0f /* sync peak tracker (one burst per 0.6 s) */
#define SSTV_SYNC_MIN_REL 0.15f /* sync burst must reach this fraction of the tracked sync peak */
#define SSTV_GAIN_CLAMP 100.0f /* max tracked gain ratio between tones (20 dB) */
#define SSTV_SYNC_MIN_MS 6 /* net dominant run (of the long sync detector) to accept a sync */
/* The long sync energy must exceed every data detector by this factor to count as a sync.
 * Was 3: on a Mac-mic recording (5.5 kHz arriving ~18 dB under the data tones, room echo
 * leaking into the start of each burst) 3x decoded 122/240 lines, 2x 207, 1.5x 227, 1.2x 240;
 * the Playdate distance recordings decode identically at every value. An adaptive version
 * (half the ratio measured on accepted bursts) settled near 3 and decoded 194: the accepted
 * part of a burst is the clean part, so it can't see the echo that breaks the start. */
#define SSTV_SYNC_DOMINANCE 1.2f

/* Derived. */
#define SSTV_SYMBOLS_PER_FRAME (SSTV_FRAME_BITS / SSTV_BITS_PER_SYMBOL) /* 164 */
#define SSTV_LEADER_SYMBOLS ((SSTV_NUM_TONES - 1) * (SSTV_LADDER_TONE_SYMBOLS + SSTV_LADDER_GAP_SYMBOLS)) /* 225 */
#define SSTV_PREAMBLE_US (SSTV_LEADER_SYMBOLS * SSTV_SYMBOL_US + SSTV_LOCK_FRAMES * SSTV_PHYS_US) /* 2.916 s */
/* Per-band timing (the audible band is SSTV_SYMBOL_US / SSTV_PHYS_US). */
#define SSTV_BAND_SYMBOL_US(band) ((band) == SSTV_BAND_HF ? SSTV_HF_SYMBOL_US : SSTV_SYMBOL_US)
#define SSTV_BAND_NUM_TONES(band) ((band) == SSTV_BAND_HF ? SSTV_HF_NUM_TONES : SSTV_NUM_TONES)
#define SSTV_BAND_BITS(band) ((band) == SSTV_BAND_HF ? SSTV_HF_BITS_PER_SYMBOL : SSTV_BITS_PER_SYMBOL)
#define SSTV_BAND_SYMBOLS_PER_FRAME(band) ((SSTV_FRAME_BITS + SSTV_BAND_BITS(band) - 1) / SSTV_BAND_BITS(band)) /* 180 / 120 */
#define SSTV_BAND_SYNC_US(band) ((band) == SSTV_BAND_HF ? SSTV_HF_SYNC_SYMBOLS * SSTV_HF_SYMBOL_US : SSTV_SYNC_US) /* 16 / 24 ms */
#define SSTV_BAND_PHYS_US(band) (SSTV_BAND_SYNC_US(band) + SSTV_BAND_SYMBOLS_PER_FRAME(band) * SSTV_BAND_SYMBOL_US(band)) /* 672000 / 354000 */
#define SSTV_BAND_LEADER_SYMBOLS(band) ((SSTV_BAND_NUM_TONES(band) - 1) * (SSTV_LADDER_TONE_SYMBOLS + SSTV_LADDER_GAP_SYMBOLS)) /* 225 / 525 */
#define SSTV_BAND_PREAMBLE_US(band) (SSTV_BAND_LEADER_SYMBOLS(band) * SSTV_BAND_SYMBOL_US(band) + SSTV_LOCK_FRAMES * SSTV_BAND_PHYS_US(band))
#define SSTV_RESUME_LEADER_US 400000 /* short re-calibration leader after a pause (the long start leader is 2 s) */
#define SSTV_RESUME_LEADER_SYMBOLS (SSTV_RESUME_LEADER_US / SSTV_SYMBOL_US) /* 100 symbols */
#define SSTV_PHYS_US (SSTV_SYNC_US + SSTV_SYMBOLS_PER_FRAME * SSTV_SYMBOL_US) /* 672000 per physical frame */
#define SSTV_LINE_US (SSTV_PHYS_US * SSTV_SEGS_PER_LINE) /* 1344000 */
