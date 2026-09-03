# SSTV for Flipper Zero → Playdate

Flipper Zero plays a 400×240 1-bit image through its speaker; a Playdate decodes it from
its microphone and draws it line by line while the sound is playing. About 3.4 minutes
per image at the default 2 ms symbols.

Signal: 2-FSK, 2000 Hz = 0, 3000 Hz = 1, 4000 Hz line sync, 2 ms per bit. Each line
carries its number and a CRC-8, so lost lines leave gaps instead of shifting the picture.
Spec and revision notes: `docs/2026-09-03-sstv-flipper-playdate-design.md`.

## Layout

- `common/` portable C: frame format, CRC, demodulator, reference modulator, PBM parser
- `tests/` host tests and WAV tools (`make test`, `make all`)
- `flipper/` FAP for Momentum firmware (`ufbt`); `flipper/lib/common` links to `common/`
- `playdate/` Playdate C app (`make`)
- `tools/` image conversion (`sh tools/setup.sh` once)
- `images/` sample images

## Build

    (cd tests && make test)            # host unit tests
    (cd playdate && make)              # SSTV_RX.pdx for device + simulator
    (cd flipper && ufbt)               # dist/sstv_tx.fap, Momentum SDK
    (cd flipper && ufbt launch)        # install and run on a connected Flipper

Momentum SDK for ufbt:

    pip install ufbt
    ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel release

## Images

    sh tools/setup.sh
    tools/.venv/bin/python tools/img2pbm.py photo.jpg images/photo.pbm

Copy `.pbm` files to the Flipper SD card at `/ext/apps_data/sstv_tx/`. The app creates
the folder on first run.

## Using it

Flipper: Apps → Tools → SSTV TX → pick a `.pbm` → OK sends, Back stops.
Playdate: open SSTV RX, allow the microphone, put the Flipper speaker 10–30 cm from the
Playdate mic (bottom edge, next to the crank side). A = clear, B = hide the status bar.

## Debugging order

1. `cd tests && make test` — pure loopback, noise, clock and frequency offsets.
2. Playdate Simulator + Mac speakers: `tools/wav_from_pbm.sh images/sphere.pbm /tmp/s.wav`,
   `afplay /tmp/s.wav`, watch the simulator draw. The simulator uses the Mac's default
   microphone.
3. Flipper into a microphone: record a WAV (QuickTime, or any recorder), then
   `tests/bin/sstv_decode_wav rec.wav out.pbm` prints `lines= good= distinct= crc_errors=`.
   `SSTV_DUMP=dump.txt` writes per-bit soft decisions for analysis.
4. Flipper → Playdate hardware, 10–30 cm apart, quiet room.

## Tuning

Protocol constants live in `common/sstv_params.h`. `SSTV_BIT_US`, `SSTV_SYNC_US`,
`SSTV_LEADER_US`, `SSTV_SAMPLE_POINT` and `SSTV_EDGE_ADJUST_US` accept `-D` overrides
(`make EXTRA_CFLAGS=-DSSTV_BIT_US=1000` in `tests/`; add the same define to
`UDEFS` in `playdate/Makefile` and `cdefines` in `flipper/application.fam`). Transmitter
and receiver must use the same values.

Measured on a Mac mini speaker → USB desk microphone channel: with 1 ms symbols the
previous bit's ringing dominated the detector (~25 % bit errors). With 2 ms symbols about
half the lines decode cleanly; the remaining errors were periodic room noise. A Flipper at
10–20 cm from the Playdate is a much shorter, cleaner path, so 1 ms may work there; the
default stays at 2 ms until measured.
