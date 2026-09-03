#pragma once

/* Image geometry: Playdate screen, 1 bit per pixel, PBM P4 packing (bit 1 = black). */
#define SSTV_WIDTH 400
#define SSTV_HEIGHT 240
#define SSTV_LINE_BYTES 50 /* ceil(400 / 8) */
#define SSTV_IMAGE_BYTES (SSTV_LINE_BYTES * SSTV_HEIGHT) /* 12000 */

/* Line frame: line number (8) + pixels (400) + CRC-8 (8). */
#define SSTV_FRAME_BYTES (1 + SSTV_LINE_BYTES + 1) /* 52 */
#define SSTV_FRAME_BITS (SSTV_FRAME_BYTES * 8) /* 416 */

/* Tones, Hz. Square waves. 1 kHz apart so a 1 ms detector window sees them as orthogonal;
 * odd harmonics (6, 9, 10, 12 kHz) miss every detected tone. */
#define SSTV_F0_HZ 2000 /* bit 0 */
#define SSTV_F1_HZ 3000 /* bit 1 */
#define SSTV_FSYNC_HZ 4000 /* line sync */

/* Timing, microseconds. Every duration is a whole number of cycles of its tone.
 *
 * The symbol is 2 ms while the detector window is 1 ms: measured speaker->room->mic
 * channels take 1-4 ms to decay by 20 dB after a tone stops, so the decision is taken over
 * the second half of the symbol, after the previous tone has died down. These may be
 * overridden with -D for experiments; transmitter and receiver must agree. */
#ifndef SSTV_BIT_US
#define SSTV_BIT_US 2000
#endif
#ifndef SSTV_SYNC_US
#define SSTV_SYNC_US 8000
#endif
#ifndef SSTV_LEADER_US
#define SSTV_LEADER_US 1000000 /* alternating 0/1 symbols before line 0 (gain calibration) */
#endif

/* Receiver tuning. */
#define SSTV_WINDOW_US 1000 /* detector window; sets tone resolution (1 kHz) */
#ifndef SSTV_SAMPLE_POINT
#define SSTV_SAMPLE_POINT 0.8f /* fraction of the symbol at which the window ends */
#endif
#ifndef SSTV_EDGE_ADJUST_US
#define SSTV_EDGE_ADJUST_US -300 /* echoes delay the sync->data crossover; pull the edge back */
#endif
#define SSTV_GAIN_TAU_S 0.3f /* per-tone peak tracker decay time constant */
#define SSTV_GAIN_CLAMP 30.0f /* max tracked gain ratio between tones (~15 dB) */

/* Derived. */
#define SSTV_LEADER_SYMBOLS (SSTV_LEADER_US / SSTV_BIT_US)
#define SSTV_LINE_US (SSTV_SYNC_US + SSTV_FRAME_BITS * SSTV_BIT_US)
