# SSTV for Flipper Zero → Playdate

Flipper Zero plays a 400×240 1-bit image through its buzzer; a Playdate decodes it from its
microphone and draws it segment by segment while the sound is playing. One pass takes about
4.7 minutes.

Signal: 4-FSK, 2 bits per 4 ms symbol on 2500/3000/3500/4000 Hz, 5500 Hz frame sync. Half-line
frames with Reed-Solomon (8 parity bytes) and CRC, four frames byte-interleaved, so a noise
burst is spread thin and repaired. The receiver tracks the symbol clock from the sync period and
flywheels through buried syncs. Design and measurement notes:
`docs/2026-09-03-sstv-flipper-playdate-design.md` (the "Current protocol"
section on top is authoritative).

## Layout

- `common/` portable C: params, frame + interleave, RS codec, demodulator, block assembler,
  reference modulator, PBM parser
- `tests/` host tests and WAV tools (`make test`, `make all`)
- `flipper/` FAP for Momentum firmware (`ufbt`); `flipper/lib/common` links to `common/`
- `playdate/` Playdate C app (`make`)
- `tools/` image conversion (`sh tools/setup.sh` once)
- `images/` sample images

## Build and install

    (cd tests && make test)            # host unit tests
    (cd flipper && ufbt launch)        # build, install and run on a connected Flipper
    (cd playdate && make)              # SSTV_RX.pdx

Momentum SDK for ufbt: `ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json --channel release`.
Playdate install: `pdutil <serial port> datadisk`, copy `SSTV_RX.pdx` into `Games/`, eject,
`pdutil <port> run /Games/SSTV_RX.pdx`. Or launch it from the Playdate's game list.

Images: `sh tools/setup.sh`, then `tools/.venv/bin/python tools/img2pbm.py photo.jpg out.pbm`
and copy the `.pbm` to the Flipper SD card at `/ext/apps_data/sstv_tx/`.

## Using it

Flipper: Apps → Tools → SSTV TX → pick a `.pbm` → OK sends one pass, Back stops.
Playdate: SSTV RX, allow the microphone, put the Flipper 10–30 cm from the Playdate mic (bottom
edge). Status bar: `good N/480 noisy M rs K fw F/L stS drop D tone`. A clears, B hides the bar,
Up records 30 s of raw microphone audio to `Data/com.negenii.sstvrx/rec.wav` for analysis.

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
