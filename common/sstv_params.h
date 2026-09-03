#pragma once

/* Image geometry: Playdate screen, 1 bit per pixel, PBM P4 packing (bit 1 = black). */
#define SSTV_WIDTH 400
#define SSTV_HEIGHT 240
#define SSTV_LINE_BYTES 50 /* ceil(400 / 8) */
#define SSTV_IMAGE_BYTES (SSTV_LINE_BYTES * SSTV_HEIGHT) /* 12000 */

/* Line frame: [line number][400 pixel bits][CRC-8] = 52 data bytes, followed by
 * RS_PARITY (12) Reed-Solomon parity bytes: 64 bytes, corrects up to 6 bad bytes. */
#define SSTV_DATA_BYTES (1 + SSTV_LINE_BYTES + 1) /* 52 */
#define SSTV_PARITY_BYTES 12
#define SSTV_FRAME_BYTES (SSTV_DATA_BYTES + SSTV_PARITY_BYTES) /* 64 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 512 */

/* Modulation: 4-FSK, 2 bits per 4 ms symbol, square-wave tones.
 *
 * Why 4 ms and four tones: on a Flipper buzzer -> Playdate microphone path the buzzer rings
 * at its ~3 kHz resonance at every frequency change and room/case tails at some frequencies
 * last several ms. With 2 ms symbols the previous symbol dominated the decision window.
 * A 4 ms symbol with the decision taken over its last 2 ms gives tails 2 ms to fall by
 * 20 dB; two bits per symbol keep the line time unchanged (1.04 s).
 *
 * Why these tones: all are multiples of 500 Hz, so they are orthogonal over the 2 ms
 * detector window (and so is the 3 kHz ringing). None is 2x or 3x another (the buzzer is
 * nonlinear: 2000/4000 put a second harmonic straight into the other detector). Measured
 * decay to -20 dB at the Playdate mic: 3500 1.8 ms, 4000 1.9, 5500 1.3, 6000 1.0 ms,
 * versus 15 ms at 2000 Hz and 5 ms at 5000 Hz. Levels are within 9 dB, which the per-tone
 * gain trackers absorb. Symbol value v (0..3) is sent as SSTV_TONE_HZ[v]. */
#define SSTV_NUM_TONES 4
#define SSTV_BITS_PER_SYMBOL 2
#define SSTV_TONE0_HZ 3500
#define SSTV_TONE1_HZ 4000
#define SSTV_TONE2_HZ 5500
#define SSTV_TONE3_HZ 6000
#define SSTV_TONES_INIT {SSTV_TONE0_HZ, SSTV_TONE1_HZ, SSTV_TONE2_HZ, SSTV_TONE3_HZ}
#define SSTV_FSYNC_HZ 5000 /* line sync; 16 ms is long enough for its slower tail */

/* Timing, microseconds. Every duration is a whole number of cycles of its tone. */
#ifndef SSTV_SYMBOL_US
#define SSTV_SYMBOL_US 4000
#endif
#ifndef SSTV_SYNC_US
#define SSTV_SYNC_US 16000
#endif
#ifndef SSTV_LEADER_US
#define SSTV_LEADER_US 1000000 /* leader: symbols cycling 0,1,2,3 (gain calibration) */
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
#define SSTV_GAIN_CLAMP 30.0f /* max tracked gain ratio between tones (~15 dB) */
#define SSTV_SYNC_MIN_MS 6 /* net dominant run (of the long sync detector) to accept a sync */

/* Derived. */
#define SSTV_SYMBOLS_PER_FRAME (SSTV_FRAME_BITS / SSTV_BITS_PER_SYMBOL) /* 256 */
#define SSTV_LEADER_SYMBOLS (SSTV_LEADER_US / SSTV_SYMBOL_US)
#define SSTV_LINE_US (SSTV_SYNC_US + SSTV_SYMBOLS_PER_FRAME * SSTV_SYMBOL_US) /* 1040000 */
