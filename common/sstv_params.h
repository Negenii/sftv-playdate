#pragma once

/* Image geometry: Playdate screen, 1 bit per pixel, PBM P4 packing (bit 1 = black). */
#define SSTV_WIDTH 400
#define SSTV_HEIGHT 240
#define SSTV_LINE_BYTES 50 /* ceil(400 / 8) */
#define SSTV_IMAGE_BYTES (SSTV_LINE_BYTES * SSTV_HEIGHT) /* 12000 */

/* Segments and frames. A line is split into SSTV_SEGS_PER_LINE segments so a burst of
 * noise costs part of a line, not all of it. Logical frame = [segment index, 2 bytes]
 * [SSTV_SEG_BYTES pixels][CRC-8] + SSTV_PARITY_BYTES Reed-Solomon parity (corrects
 * PARITY/2 unknown bytes, or PARITY erased ones). SSTV_ILEAVE consecutive logical frames
 * are byte-interleaved into SSTV_ILEAVE physical frames, so a noise burst hitting one
 * physical frame is spread thinly over SSTV_ILEAVE logical ones. */
#ifndef SSTV_SEGS_PER_LINE
#define SSTV_SEGS_PER_LINE 2 /* 200 px segments */
#endif
#define SSTV_SEG_PX (SSTV_WIDTH / SSTV_SEGS_PER_LINE)
#define SSTV_SEG_BYTES (SSTV_SEG_PX / 8) /* 25 */
#define SSTV_NUM_SEGS (SSTV_HEIGHT * SSTV_SEGS_PER_LINE) /* 480 */
#define SSTV_DATA_BYTES (2 + SSTV_SEG_BYTES + 1) /* 28 */
#ifndef SSTV_PARITY_BYTES
#define SSTV_PARITY_BYTES 8
#endif
#define SSTV_FRAME_BYTES (SSTV_DATA_BYTES + SSTV_PARITY_BYTES) /* 36 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 288 */
#ifndef SSTV_ILEAVE
#define SSTV_ILEAVE 4
#endif
#define SSTV_NUM_BLOCKS (SSTV_NUM_SEGS / SSTV_ILEAVE) /* 120 */

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
#define SSTV_TONE0_HZ 2500
#define SSTV_TONE1_HZ 3000
#define SSTV_TONE2_HZ 3500
#define SSTV_TONE3_HZ 4000
#define SSTV_TONES_INIT {SSTV_TONE0_HZ, SSTV_TONE1_HZ, SSTV_TONE2_HZ, SSTV_TONE3_HZ}
/* Transmit pre-emphasis: PWM duty per tone, percent. The fundamental of a square wave scales
 * with sin(pi * duty), so a narrow pulse attenuates a tone without touching the others:
 * 12 % = -10 dB, 30 % = -4 dB. The buzzer's 3 kHz resonance arrived 10-27 dB louder than
 * the other tones at a Playdate microphone; this evens the set out (and quietens the whine). */
#define SSTV_TONE_DUTY_PCT_INIT {30, 12, 30, 50}
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
#define SSTV_GAIN_CLAMP 100.0f /* max tracked gain ratio between tones (20 dB) */
#define SSTV_SYNC_MIN_MS 6 /* net dominant run (of the long sync detector) to accept a sync */

/* Derived. */
#define SSTV_SYMBOLS_PER_FRAME (SSTV_FRAME_BITS / SSTV_BITS_PER_SYMBOL) /* 144 */
#define SSTV_LEADER_SYMBOLS (SSTV_LEADER_US / SSTV_SYMBOL_US)
#define SSTV_PHYS_US (SSTV_SYNC_US + SSTV_SYMBOLS_PER_FRAME * SSTV_SYMBOL_US) /* 592000 per physical frame */
#define SSTV_LINE_US (SSTV_PHYS_US * SSTV_SEGS_PER_LINE) /* 1184000 */
