# SFTV, Slow Flipper TV: Flipper Zero → Playdate pictures over sound

Flipper Zero plays a 400×240 1-bit image through its buzzer; a Playdate decodes it from its
microphone and draws it line by line while the sound is playing. One pass takes 1.5–5.4 minutes
depending on how well the image compresses (dithered photos: 5.4 min; line art: much less).

Signal: 4-FSK, 2 bits per 4 ms symbol; symbol 0 is silence, 1..3 are 3000/3500/4000 Hz,
5500 Hz frame sync. Frames are 41 bytes: header (type + index), 26 payload bytes, CRC-8 and
12 Reed-Solomon parity bytes; four frames are byte-interleaved so a noise burst is spread thin
and repaired. A pass starts with a META frame carrying the file name, the pass length in frames and the mode, then a 40x20 dithered preview in one
block (0.67 s) and an 80x48 one in five (progressive, 4 s total) so the receiver can show what is coming, and ends with an END META frame. Pixels go either as RAW half-line segments (a lost frame costs half a line) or, when the
lossless line codec saves at least 15 %, as CODED blocks; the transmitter picks per image. The
receiver tracks the symbol clock from the sync period and flywheels through buried syncs. The
Flipper LED follows the data (blue = tone, off = silence, red = sync); on its screen the
picture is a framed sheet that slides out through the top edge as lines go out (uncovering a
resting dolphin), and on the Playdate the same sheet crawls in from the bottom edge at the real
line rate (coded passes announce their per-block line schedule). Design and measurement notes:
`docs/2026-09-03-sstv-flipper-playdate-design.md` (the "Current protocol"
section on top is authoritative).

## Layout

- `common/` portable C: params, line codec, frame + block + interleave, RS codec, demodulator,
  block assembler, reference modulator, PBM parser
- `tests/` host tests and WAV tools (`make test`, `make all`)
- `flipper/` FAP for Momentum firmware (`ufbt`); `flipper/lib/common` links to `common/`
- `playdate/` Playdate C app (`make`)
- `tools/` image conversion (`sh tools/setup.sh` once)
- `images/` sample images

## Build and install

    (cd tests && make test)            # host unit tests
    (cd flipper && ufbt launch)        # build, install and run on a connected Flipper
    (cd playdate && make)              # SFTV_RX.pdx

Momentum SDK for ufbt: `ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel release`.
Playdate install: `pdutil <serial port> datadisk`, copy `SFTV_RX.pdx` into `Games/`, eject,
`pdutil <port> run /Games/SFTV_RX.pdx`. Or launch it from the Playdate's game list. The SDK
Makefile drops object files into `playdate/common/`; `rm -rf playdate/common playdate/build`
forces a full rebuild.

Images: `sh tools/setup.sh`, then `tools/.venv/bin/python tools/img2pbm.py photo.jpg out.pbm`
and copy the `.pbm` to the Flipper SD card at `/ext/apps_data/sftv_tx/`.

## Using it

Flipper: Apps → Tools → SFTV TX → a tile grid of the `.pbm` files in `apps_data/sftv_tx` (Left/Right, OK) → OK sends one pass, Back stops.
Playdate: SFTV RX, allow the microphone, put the Flipper 10–30 cm from the Playdate mic (bottom
edge). Main menu: Receive or Gallery. Receive shows the dolphin sending the preview to a
Playdate; the picture then rises from the bottom edge line by line over the scene, lost
half-lines showing a dimmed preview placeholder. An
incoming transmission jumps to Receive from any screen. A dot in the corner flashes per frame.
Right toggles the side panel (name, state and ZIP/RAW pills, line progress, counters, tone
level). When the END frame arrives a banner shows the file name and line count, and the image
is saved as `Data/com.negenii.sftvrx/<name>.pbm` (same name overwrites). Gallery: a grid of
thumbnails, d-pad moves the cursor, A opens full screen, B goes back. A in receive mode clears
the image, B returns to the menu, Up records 30 s of raw microphone audio to `rec.wav`.

The dolphin sprites are cropped from the Flipper firmware's `L1_Furippa1` (Playdate) and
`L1_Waves` (Flipper) animations (GPL-3, flipperdevices/flipperzero-firmware) as placeholders.

## Debugging

1. `cd tests && make test` — loopback with white noise, noise bursts, clock offsets.
2. Record on the Playdate (Up), fetch `rec.wav` over the data disk, then
   `tests/bin/sstv_decode_wav rec.wav out.pbm` prints frames, flywheel count, blocks, segments.
   `SSTV_DUMP=file` writes raw physical frames and soft bits.
3. Protocol constants live in `common/sstv_params.h`; transmitter and receiver must agree.

## Findings worth keeping

- The buzzer rings at ~3 kHz on every frequency change; 2000–2250 Hz and 4750–5000 Hz tails
  last 5–15 ms; high tones (5.5–6 kHz) swing 10–20 dB with device position.
- Flipper and Playdate sample clocks differ by about 0.1 %.
- A per-PWM-cycle ISR on the Flipper loses overflows under interrupt latency; use the timer's
  repetition counter.
- The Playdate must remove its mic callback on terminate, or the next launch crashes.
