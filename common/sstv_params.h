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
#define SSTV_NOTE_MAX_CHARS 200
#define SSTV_NOTE_CHUNK SSTV_SEG_BYTES /* 25 ASCII bytes per RAW text frame */
#define SSTV_NOTE_MAX_FRAMES 8 /* ceil(200/25) = 8, already a multiple of SSTV_ILEAVE */
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
#ifndef SSTV_PARITY_BYTES
#define SSTV_PARITY_BYTES 12
#endif
#define SSTV_FRAME_BYTES (SSTV_DATA_BYTES + SSTV_PARITY_BYTES) /* 41 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 328 */
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

/* Timing, microseconds. Every duration is a whole number of cycles of its tone. */
#ifndef SSTV_SYMBOL_US
#define SSTV_SYMBOL_US 4000
#endif
#ifndef SSTV_SYNC_US
#define SSTV_SYNC_US 16000
#endif
#ifndef SSTV_LEADER_US
#define SSTV_LEADER_US 2000000 /* leader: symbols cycling 0,1,2,3 (gain calibration); 2 s so the receiver settles before the first frame (START) */
#endif

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

/* Derived. */
#define SSTV_SYMBOLS_PER_FRAME (SSTV_FRAME_BITS / SSTV_BITS_PER_SYMBOL) /* 164 */
#define SSTV_LEADER_SYMBOLS (SSTV_LEADER_US / SSTV_SYMBOL_US)
#define SSTV_RESUME_LEADER_US 400000 /* short re-calibration leader after a pause (the long start leader is 2 s) */
#define SSTV_RESUME_LEADER_SYMBOLS (SSTV_RESUME_LEADER_US / SSTV_SYMBOL_US) /* 100 symbols */
#define SSTV_PHYS_US (SSTV_SYNC_US + SSTV_SYMBOLS_PER_FRAME * SSTV_SYMBOL_US) /* 672000 per physical frame */
#define SSTV_LINE_US (SSTV_PHYS_US * SSTV_SEGS_PER_LINE) /* 1344000 */
