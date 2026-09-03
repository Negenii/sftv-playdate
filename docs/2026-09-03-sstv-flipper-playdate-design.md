# SSTV-style image transfer: Flipper Zero → Playdate over sound

Date: 2026-09-03
Status: approved design

## Current protocol (revision 6, 2026-09-03) — supersedes the signal sections below

Everything below this section is the original design plus revision notes; the sections
"Signal format", "Line frame" and the demodulator algorithm are historical. What runs now:

- **Modulation**: 4-FSK, 2 bits per 4 ms symbol. Symbol 0 is silence (0 % PWM duty), symbols
  1..3 are square-wave tones 3000 / 3500 / 4000 Hz; frame sync 5500 Hz for 16 ms. Tones sit on
  a 500 Hz grid so a 2 ms detector window sees them (and the buzzer's 3 kHz ringing) as
  orthogonal; none is 2x or 3x another (the buzzer is nonlinear). Per-tone PWM duty on the
  Flipper (12 / 30 / 50 %) attenuates the loud resonance tone so the tones arrive within a few
  dB of each other at the Playdate. 2500 Hz was dropped: it had only ~5 dB present/absent
  contrast and read as silence a third of the time. White image areas are quiet; only drawn
  pixels sound, plus a short sync beep every 0.67 s.
- **Why 4 ms**: measured tone tails last 1-2 ms (2000-2250 and 4750-5000 Hz ring for 5-15 ms
  and are avoided). The decision window is the last 2 ms of the symbol.
- **Frames**: logical frame = [2-byte header][26 payload bytes][CRC-8] + 12 Reed-Solomon
  parity bytes (41 bytes = 164 symbols). Header: top 2 bits frame type, 14-bit index. Four
  logical frames are byte-interleaved into four physical frames of sync + 164 symbols = 672 ms,
  so a noise burst is spread across four codewords. Frame types:
  - `META` (2): payload [subtype][data]. Subtype 0 = START (data = file name without extension,
    up to 24 chars), sent as the first block of every pass; subtype 1 = END, sent as the last
    block; subtype 2 = PREVIEW, 20 independent frames (index = chunk, 25 bytes each) right after
    START carrying an 80x48 preview of the picture: each preview pixel is the black count of a
    5x5 block, Bayer-dithered. 5 blocks = 3.4 s (+4 % on the checker image, +1 % on a photo).
    The receiver starts a new image on START (saving the previous one), shows the preview
    filling in and uses it, 5x upscaled and drawn as a 50 % checker, as a placeholder under
    the picture until real lines replace it; on END it saves and announces.
  - `RAW` (0): index = half-line segment 0..479, payload = 25 bytes of pixels + 1 spare. Every
    frame stands alone, so a lost frame costs half a line, never a block.
  - `CODED` (1): index = block number; the four frames of a block carry a 104-byte block
    [count][first line][records], each record a line coded losslessly (raw, run lengths, or
    run lengths of the XOR with the previous line; the shortest wins). Lines unpack up to the
    first failed frame.
  The transmitter builds both layouts and sends CODED only if it needs at most 85 % of the
  RAW frame count (`SSTV_CODED_MIN_GAIN`); dithered photos go RAW (480 frames, 5.4 min), line
  art CODED (checker image 30 blocks, 1.4 min). One pass; OK on the Flipper repeats.
- **Receiver**: quadrature detectors (2 ms window) for the tones and the sync plus a 4 x longer
  sync detector; per-tone peak-tracking gain normalisation (clamp 20 dB); symbol = argmax of
  normalised energies against a fixed silence level (0.3); soft confidence per bit. Sync:
  dominance run of the long detector gated by the tracked sync peak, then the edge where the
  sync energy falls to a quarter of the burst peak; a second-order tracking loop starts each
  frame halfway between predicted and measured edge and nudges the symbol clock (the two
  devices' clocks differ by 0.1 %). Flywheel: a sync buried in noise is replaced by the
  predicted edge for up to 3 frames; mid-frame false syncs are rejected; weak bursts do not
  move the prediction. The block layer de-interleaves four physical frames, marks the least
  confident bytes as erasures and decodes RS errors-and-erasures (up to 12 erased bytes per
  frame), checks CRC, then dispatches by frame type.
- **Transmitter**: TIM16 driven through the repetition counter and preloaded registers, one
  interrupt per symbol. The RGB LED follows the symbol on the speaker (off for silence, blue
  for tones, red during sync) from a 2 ms polling thread. The preview image inverts line by
  line from the top as lines go out, so the picture is its own progress bar.
- **Playdate UI**: main menu with two rounded tiles, Receive (Flipper dolphin, GPL-3 sprite
  from the firmware's `L3_Hijack_radio` animation, to be replaced by the user's own drawing)
  and Gallery (thumbnail of the latest saved picture). Receive shows an intro scene: the
  dolphin, a drawn Playdate, and once START arrives a water spout from the blowhole to the
  console whose screen fills with the preview chunk by chunk; the first picture line switches
  to the full-screen picture over the dimmed preview placeholder. Whatever screen is up, START
  jumps to Receive. A dot in the corner flashes per frame. Right toggles a side panel (Roobert
  10 Bold, numbers in Roobert 9 Mono): name, state pill (IDLE / RX / LOST / DONE), ZIP or RAW
  pill, REC pill, line progress bar, right-aligned counters, tone and level bar. On END a
  banner names the file and the line count. Pictures are saved as P4 PBM under
  `Data/<name>.pbm` (same name overwrites; never-received segments are written white) on
  END, on the next START, on a 5 s signal loss and on exit. Gallery: 3 x 2 grid of 100 x 60
  rounded thumbnails with a cursor, A opens one full screen, B back; file extensions are never
  shown. A in receive mode clears, B returns to the menu, Up records 30 s of microphone audio.
- **Measured**: loopback white noise 0 dB SNR 227-232/240 lines, 6 dB 240/240; 400 ms noise
  bursts at +15 dB every 3 s 240/240; 700 ms bursts 192/240; clock offset ±1500 ppm 240 and
  234. Hardware: checker image complete 240/240 in one pass (1.4 min); dithered photo RAW in
  5.4 min with a few half-lines lost when the devices are moved.

## Goal

A proof of concept: Flipper Zero plays a 400×240 1-bit image as sound through its
built-in speaker; a Playdate listens through its microphone and draws the image
line by line, live, while the transmission is playing. One direction only.

Flipper shows a shrunken preview of the image on its own 128×64 screen and a
progress indicator while transmitting.

## Non-goals

- Return channel Playdate → Flipper (the GPIO mic on Flipper is not used).
- Grayscale / analog SSTV. The image is 1 bit per pixel.
- Selectable resolution. Fixed 400×240.
- Error correction or retransmission. Corrupt lines are drawn as received.
- Saving the received image on Playdate.

## Signal format

### Physical layer

| Symbol  | Tone    | Duration            |
|---------|---------|---------------------|
| bit 0   | 2000 Hz | 2 ms (4 cycles)     |
| bit 1   | 3000 Hz | 2 ms (6 cycles)     |
| sync    | 4000 Hz | 8 ms (32 cycles)    |
| leader  | alternating bit 0 / bit 1 symbols | 1000 ms, once at start of image |

Bit rate 500 baud. The detector window is 1 ms, so the tones (1 kHz apart) are
orthogonal to it; the decision is taken over the second half of each 2 ms symbol.
Square-wave harmonics (6, 9, 12 kHz) do not land on any detected tone.

Revision note (2026-09-03, after measuring a real speaker → room → microphone channel):
the first design used 1 ms symbols. Measured channels take 1–4 ms to decay by 20 dB
after a tone stops and about 1 ms for a new tone to rise, so with 1 ms symbols the
previous bit dominated the detector (contrast 1.7 dB, ~25 % bit errors). With 2 ms
symbols and the window over the symbol tail the same channel decodes about half of
the lines cleanly; the rest is periodic room noise. The symbol, sync and leader
durations are `#ifndef`-overridable in `sstv_params.h` for experiments (transmitter
and receiver must agree). The alternating leader lets the receiver's per-tone gain
trackers settle before line 0.

Tones are square waves (Flipper speaker is a PWM output). Each bit is a whole
number of cycles so the transmitter can switch frequency at a cycle boundary with
continuous phase.

### Line frame

```
[sync 4000 Hz 8 ms] [line number, 8 bits] [400 pixel bits] [CRC-8, 8 bits]
```

- Bits are sent MSB first.
- Line number 0..239. Pixel bit 1 = black (matches PBM P4 convention).
- CRC-8 (poly 0x07, init 0x00) over the 51 bytes: line number + 50 pixel bytes.
- Line duration: 8 ms sync + 416 × 2 ms = 840 ms. Full image: 1 s leader + 240 × 840 ms
  ≈ 203 s (3.4 min).
- Lines are sent in order 0..239 without gaps. The receiver does not rely on the
  order: each line carries its own number, so a lost line leaves a gap and does
  not shift the rest of the image.
- CRC is informational: the receiver draws every line whose sync and 416 bits
  were read, and counts CRC failures for the status display.

### End of image

After line 239 the transmitter stops (silence). The receiver shows "complete" when
all 240 lines were seen, or when no line arrived for 5 s and a line ≥ 230 was seen;
a 5 s gap earlier in the image shows "signal lost". The picture stays on screen.
A row that was received with a good CRC is never overwritten by a later copy with a
bad CRC (that is a corrupted line number, not a retransmission).

## Repository layout

```
Flipper/                       (this repo)
  common/                      portable C, no platform dependencies
    crc8.[ch]                  CRC-8 poly 0x07
    sstv_frame.[ch]            line frame pack/unpack (bit order, CRC)
    sstv_params.h              tone frequencies, bit period, image size
    sstv_demod.[ch]            sample-stream demodulator → decoded lines
    sstv_modem_ref.[ch]        reference software modulator (square-wave samples)
  tests/                       host unit tests, plain C + minimal assert macros
    Makefile                   `make test` builds and runs with clang
  flipper/                     FAP for Momentum (ufbt)
    application.fam
    sstv_app.c                 scene flow, file loading, preview
    sstv_tx.[ch]               TIM16 driver, ISR bit clock
    icons/
  playdate/                    C app
    Makefile
    src/main.c                 event handler, update loop, UI
    src/mic_ring.[ch]          SPSC ring buffer fed from mic callback
  tools/
    img2pbm.py                 any image → 400×240 dithered P4 .pbm
    wav_from_pbm.py            .pbm → WAV via the reference modulator (for testing
                               the Playdate receiver from a Mac speaker)
  images/                      two or three sample .pbm files
  docs/      this document
```

## common/

### sstv_params.h

Constants: `SSTV_WIDTH 400`, `SSTV_HEIGHT 240`, `SSTV_F0 2000`, `SSTV_F1 3000`,
`SSTV_FSYNC 4000`, `SSTV_BIT_US 1000`, `SSTV_SYNC_US 5000`, `SSTV_LEADER_US
1000000`, `SSTV_LINE_BYTES 50`, `SSTV_LINE_BITS 416` (8 + 400 + 8).

### sstv_frame

- `void sstv_frame_pack(uint8_t line_no, const uint8_t pixels[50], uint8_t out[52])`
  → line number, 50 pixel bytes, CRC.
- `bool sstv_frame_unpack(const uint8_t in[52], uint8_t* line_no, uint8_t pixels[50])`
  → returns CRC ok; fills outputs regardless.
- Bit iteration helper: `int sstv_frame_bit(const uint8_t frame[52], int index)`
  returns bit `index` (0..415), MSB first within each byte. The transmitter uses
  it to walk a frame bit by bit from an ISR.

### sstv_demod

Pure sample-stream demodulator, parametrised by sample rate.

```c
typedef void (*sstv_line_cb)(void* ctx, uint8_t line_no, const uint8_t pixels[50], bool crc_ok);
void sstv_demod_init(sstv_demod* d, float sample_rate, sstv_line_cb cb, void* ctx);
void sstv_demod_feed(sstv_demod* d, const int16_t* samples, int n);
void sstv_demod_status(const sstv_demod* d, sstv_demod_status_t* out);  // level, current tone, lines, crc errors
```

Algorithm:

1. Three quadrature detectors (2000, 3000, 4000 Hz) with a sliding window of
   `W = round(sample_rate / 1000)` samples (44 at 44.1 kHz), i.e. 1 ms regardless of
   the symbol length. Each detector keeps
   running I and Q sums using cos/sin lookup tables of length equal to the tone
   period in samples (recomputed at init for the given sample rate; use a
   phase accumulator with a 1024-entry sine table to support non-integer
   periods). Energy `E = I² + Q²` per detector, updated per sample by adding the
   new sample's contribution and subtracting the one that left the window.
2. States: `IDLE` → `SYNC` → `DATA`.
   - IDLE: a run counter increments on samples where `E_sync` exceeds both `E0` and
     `E1` by a factor of 3 and exceeds a noise floor, and decrements otherwise (so
     noise dropouts do not reset it). When it reaches 3 ms worth of samples, enter SYNC.
   - SYNC: wait for the crossover `E_sync < max(E0, E1)`; that fixes `t0`; enter DATA.
   - DATA: bit `k` (0..415) is decided when the window ends at
     `t0 + k × bit_len + SAMPLE_POINT × bit_len` (SAMPLE_POINT = 0.8, i.e. the window
     covers roughly the second half of the symbol, after the previous tone has decayed).
     `t0` is the crossover point minus W/2 plus EDGE_ADJUST (−0.3 ms: echoes delay the
     crossover). Decision: `E1 / g1 > E0 / g0` where `g0`, `g1` are per-tone peak
     trackers (decay 0.3 s, ratio clamped to ±15 dB) that absorb speaker/microphone
     coloration. After 416 bits, unpack the frame, invoke the callback, return to IDLE.
   - A sync burst detected during DATA (E_sync dominant for ≥ 3 ms) aborts the
     current line and restarts SYNC, so a garbled line does not swallow the next.
3. Status for UI: `level` = max energy normalised to full scale, `tone` = index of
   the dominant detector or none, counters for lines and CRC errors.

### sstv_modem_ref

Reference software modulator producing int16 samples of what the Flipper emits:

- `sstv_mod_init(mod, sample_rate, amplitude)`
- `sstv_mod_tone(mod, freq_hz, duration_us, int16_t* out, int cap) → n`
  square wave, phase continuous across calls.
- `sstv_mod_frame(mod, const uint8_t frame[52], out, cap) → n`
- `sstv_mod_image(mod, const uint8_t* pbm_bits, out, cap) → n`
  leader + 240 frames.

Used by the tests and by `tools/wav_from_pbm.py` (through a tiny C CLI in
`tests/` that writes a WAV, or the Python script re-implements the same
square-wave generator; the C CLI is preferred so both sides share one source).

## tests/

Plain C, compiled with clang on macOS, a minimal `CHECK()` macro, `make test`
runs everything and exits non-zero on failure.

- `test_crc8.c`: known vectors ("123456789" → 0xF4 for poly 0x07 init 0).
- `test_frame.c`: pack/unpack round trip; bit iteration order; CRC detects a
  flipped bit.
- `test_demod.c`:
  - loopback: modulate one line at 44.1 kHz, feed to demod, expect identical bits
    and `crc_ok`.
  - full image loopback: 240 lines, all received in order.
  - added white noise at SNR 10 dB: ≥ 95 % lines CRC ok.
  - frequency offsets: modulate with f0/f1/fsync each shifted by +1.5 %
    (Flipper timer rounding), expect all lines ok.
  - clock offset: modulate at 44 056 Hz (−0.1 %) but demodulate assuming 44 100,
    expect all lines ok.
  - sample rate 22 050 Hz loopback (proves the parametrisation; 8 kHz is unusable because
    4000 Hz sits at Nyquist; not used by any
    device today).
  - lost sync: cut the first 2 ms of one line's sync burst, expect that line
    missing and the following line intact.

## flipper/

Momentum firmware, SDK `mntm-012` (API 87.1), built with `ufbt` using
`--index-url=https://up.momentum-fw.dev/firmware/directory.json`.

### Scenes

1. **File select**: `dialog_file_browser_show` on `/ext/apps_data/sstv/`,
   extension filter `.pbm`. Back exits the app.
2. **Preview**: shows the image shrunk to 128×64 by nearest-neighbour sampling
   (every 400/128th column, every 240/64th row) and the file name. OK → transmit,
   Back → file select.
3. **Transmit**: preview stays on screen; a progress bar and text
   `line 120/240` at the bottom. Updated from the main thread once per line, on
   a message from the TX driver. Back stops transmission and returns to preview.
   When line 239 finishes, text changes to `done`, OK restarts, Back → preview.

### Image loading

Parse PBM P4: magic `P4`, whitespace, width, height, single whitespace, then
`ceil(w/8) × h` bytes. Reject anything that is not exactly 400×240 with a
dialog message. Keep the 12 000 bytes in heap.

### sstv_tx (transmitter)

- `bool sstv_tx_start(const uint8_t* image_bits, sstv_tx_progress_cb cb, void* ctx)`
- `void sstv_tx_stop(void)`

Implementation:

1. `furi_hal_speaker_acquire(1000)` for ownership (keeps other apps and the
   notification service off the speaker). Never call `furi_hal_speaker_start`
   afterwards; the timer is driven directly.
2. `furi_hal_bus_enable(FuriHalBusTIM16)`, configure TIM16 with LL: prescaler 0
   (64 MHz), PWM1 on CH1, ARPE enabled, output enabled, update interrupt enabled.
   Speaker pin is already routed to TIM16 CH1 by the firmware's speaker init; if
   not, configure `gpio_speaker` to its alternate function.
3. `furi_hal_interrupt_set_isr(FuriHalInterruptIdTim1UpTim16, isr, ctx)`.
4. ARR values: 2000 Hz → 31999, 3000 Hz → 21332 (3000.05 Hz), 4000 Hz → 15999.
   CCR1 = ARR / 2 (50 % duty, loudest).
5. ISR on each update event: check `LL_TIM_IsActiveFlag_UPDATE(TIM16)`, clear it,
   decrement `cycles_left`. When it reaches zero, advance the symbol sequencer
   and write the next ARR and CCR1 (preloaded, applied at the next period).
   The sequencer walks: leader (3000 cycles at 3000 Hz) → for each line: sync
   (20 cycles at 4000 Hz) → 416 bits (2 or 3 cycles each, from
   `sstv_frame_bit`) → next line. Frames are pre-packed per line into a 52-byte
   buffer by the ISR at line start (pack is cheap; or pre-pack all 240 lines =
   12.5 KB at start, simpler, do that).
   At end of a line, the ISR posts the line number to a FuriMessageQueue; the
   main thread updates the UI. After line 239 the ISR disables the timer output
   and posts a done event.
   The leader is `SSTV_LEADER_SYMBOLS` alternating bit-0 / bit-1 symbols.
6. Stop: disable interrupt, `furi_hal_interrupt_set_isr(..., NULL, NULL)`,
   disable counter and outputs, `furi_hal_bus_disable(FuriHalBusTIM16)`,
   `furi_hal_speaker_release()`.

Volume: no user control. Timer duty fixed at 50 %.

Risk noted: if TIM1 update interrupts fire on the shared vector while another
peripheral uses TIM1, the ISR ignores them (checks TIM16's flag only). If the
firmware holds the vector and `furi_hal_interrupt_set_isr` asserts, fallback is
a highest-priority thread with `furi_hal_cortex_timer_wait` per bit and
`LL_TIM_SetAutoReload`; accept GUI starvation for the 102 s.

## playdate/

C project built with the SDK Makefile (`common.mk`), SDK 3.1.1, target device
and simulator.

### Flow

1. `kEventInit`: set update callback at 50 fps, allocate a 400×240 LCDBitmap,
   clear white, call `requestMicAccess("Receive images over sound", cb, ctx)`.
   If reply is `kAccessAllow` or the callback later grants, start the mic with
   `setMicCallback(mic_cb, ctx, kMicInputAutodetect)`. If denied, show a message.
2. `mic_cb`: copy samples into an SPSC ring buffer (capacity 16 384 samples,
   volatile head/tail, single producer single consumer, no locks). Return 1.
3. `update()`: drain the ring buffer into `sstv_demod_feed`. Line callback writes
   50 bytes into the bitmap row via `getBitmapData` (rowbytes 52; Playdate bit 1
   = white, PBM bit 1 = black, so bytes are inverted on copy) and marks it dirty.
   Then `drawBitmap` at (0, 0), a 20 px status strip is drawn over the bottom
   with a filled rect: `lines 120/240  crc err 3  lvl ████  tone 3k`.
   Signal bar reflects `level` from demod status.
4. Buttons: A clears the bitmap and demod counters. B toggles the status strip.
5. End of image: when 240 distinct lines are received or 3 s pass with no sync
   after at least one line, status shows `complete` (or `complete, N missing`).

### Simulator

The Playdate Simulator passes the Mac microphone through to `setMicCallback`,
which lets the receiver be tested against a WAV played from Mac speakers before
the Flipper side exists.

## tools/

- `img2pbm.py in.jpg out.pbm`: Pillow; convert to L, resize to cover 400×240,
  centre-crop, Floyd–Steinberg dither to 1 bit, save P4.
- `wav_from_pbm.py in.pbm out.wav`: runs the `tests/sstv_wav` C tool (built by
  the tests Makefile) to produce a 44.1 kHz 16-bit mono WAV of the full
  transmission, for playing from a Mac to the Playdate.

## Debugging order

1. `make test` in `tests/` on the Mac.
2. Playdate Simulator + WAV from `wav_from_pbm.py` played through Mac speakers.
3. Flipper transmitting into the Mac microphone; record a WAV, feed it to the
   host demodulator via `tests/sstv_decode_wav` (WAV in → PBM out + stats).
4. Flipper → Playdate hardware, 10–30 cm apart.

## Open parameters to tune on hardware

- Detector dominance factor (3×) and noise floor.
- Sync minimum run (3 ms) vs false alarms.
- `SSTV_SAMPLE_POINT`, `SSTV_EDGE_ADJUST_US`, and the symbol length itself; see
  `tests/bin/sstv_decode_wav in.wav out.pbm [sample_point] [edge_adjust_samples]` and
  `SSTV_DUMP=file` for per-bit soft decisions.
- Distance and Flipper speaker volume; Playdate mic gain is fixed by hardware.
