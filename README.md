# SFTV RX: Slow Flipper TV for Playdate

SFTV RX receives pictures, notes and Flipper animations that a Flipper Zero running
[SFTV TX](https://github.com/Negenii/sftv-flipper) plays as near-ultrasound (17–21 kHz). It
listens with the Playdate's microphone and draws the picture line by line while it is still
on the air.

## Using it

- Receive asks for the microphone the first time, then listens. Put the Flipper 5–15 cm
  from the Playdate's microphone (bottom edge). An incoming transmission takes over the screen
  from anywhere in the app. A clears the screen, B goes back, Right shows a diagnostic panel.
- Gallery keeps everything received, newest first. The system menu's *filter* shows only
  images, animations or notes. A opens an item, Left/Right step through them. In an
  animation the crank takes over playback: one turn a second is 30 fps, either way.
- Every picture is also saved as PNG and every animation as GIF in
  `Data/com.negenii.sftvrx/` (connect the Playdate in disk mode to copy them).
- *about* and *support* are in the system menu.

## Building

Needs the [Playdate SDK](https://play.date/dev/) (C API):

    tools/playdate_build.sh            # build SFTV_RX.pdx, bumping playdate/build_number
    tools/playdate_build.sh install    # also copy it to a Playdate on USB and start it

Plain `cd playdate && make` works too. Host tests of the shared protocol code:
`cd tests && make test`.

Layout: `playdate/` the app, `common/` the protocol in portable C (shared with SFTV TX),
`tests/` host tests and WAV tools, `tools/` asset generators (launcher art, fonts, QR codes),
`docs/` design notes.

## Protocol

8-FSK, 17.0–20.5 kHz, 3 ms symbols, synced by a tone pattern. 45-byte frames with
Reed-Solomon parity, interleaved in blocks of four, plus an outer Reed-Solomon code across
groups of 108 frames, so a lost frame comes back without a resend. The details are in `docs/`
and `common/sstv_params.h`; transmitter and receiver must run the same protocol revision (the
major version, 3.x here).

## Credits and licenses

© 2026 negenii. The code is released under the GNU General Public License v3.0 (see
`LICENSE`). Some assets carry their own terms:

- Dolphin art (the receive scene, the launcher card and launch animation) comes from the
  Flipper Zero firmware, GPL-3.0.
- The Roobert fonts ship with the Playdate SDK by Panic and are not part of this repository:
  the build copies them from the installed SDK into the app, as the SDK's terms allow.
- The notes font (`Notes-14`) is generated from HaxrCorp 4089 by sahwar
  (`tools/haxrcorp4089.ttf`), CC BY-SA 3.0; the generated font is under the same license.
- The Buy Me a Coffee button is their brand asset, used to link to the support page.

If you like it, you can [buy me a coffee](https://buymeacoffee.com/low.effort).
