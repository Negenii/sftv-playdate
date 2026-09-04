# SFTV pause / resume

Date: 2026-09-04
Status: implemented
Base: `2026-09-03-sstv-flipper-playdate-design.md`, `2026-09-03-sstv-outer-code-design.md`

## Goal

Let the person at the Flipper pause a transmission and resume it later from the same place, so
a long transfer can be interrupted without starting over. The pause is part of the protocol:
the receiver is told, freezes its animation and shows "Paused", then continues from the
announced frame on resume.

## Control frames

Two new META subtypes (payload byte 0), sent the same way as any other frame (packed, inner
RS, interleaved into a 4-frame block, between sync bursts):

- `SSTV_META_PAUSE` (5): payload[1..2] = the frame index the sender paused at (little-endian).
- `SSTV_META_RESUME` (6): payload[1..2] = the next physical frame index the sender will play.

Both are transmitted as a block of 4 identical logical frames (header `META`, index 0). They
are control only: the receiver does not store them as data positions and they are not part of
any outer-code group, so the position/parity accounting is untouched. Their extra wall-clock
time is exactly what the RESUME index lets the receiver correct for.

## Transmitter (`flipper/sstv_tx.c`)

The sequencer plays the prebuilt physical frames from `tx.frames` via `tx.line`. Pause/resume
inject a prebuilt control block without disturbing that buffer:

- `sstv_tx_pause()` (app thread): builds the PAUSE block into `tx.ctrl` (RS encode off the ISR)
  and sets `pause_pending`. At the next data-frame boundary the ISR plays the 4 PAUSE frames,
  then enters `PhasePaused` and holds the speaker silent (tone 0). `tx.line` does not advance.
- `sstv_tx_resume()` (app thread): builds the RESUME block carrying `tx.line`, sets
  `resume_pending`. From the paused hold the ISR sends a short re-calibration leader
  (`SSTV_RESUME_LEADER_SYMBOLS`, 0.4 s), then the 4 RESUME frames, then continues the pass from
  `tx.line`.
- `sstv_tx_is_paused()` reports the hold. The control blocks are counted in neither `tx.total`
  nor the progress percentage.

Building the control block on the caller's thread keeps the RS encode out of the 2 ms ISR
budget; the ISR only ever reads bytes and toggles a couple of flags.

## Flipper UI (`flipper/sstv_app.c`)

During `StateTransmit`: **Back** pauses (was: stop). While paused, **Back** stops and returns
to the preview, **OK** resumes. The panel shows "Paused" and the hints switch to
"OK resume / Back stop". The LED thread refreshes the panel ~5x/s so the paused label appears
even though no frame events arrive during the hold.

## Receiver (`common/sstv_rx.c`, `playdate/src/main.c`)

- `sstv_rx_set_control_cb()` registers `sstv_control_cb(ctx, subtype, arg)`. `handle_frame`
  calls it for PAUSE/RESUME and returns without storing.
- Playdate: on PAUSE it sets `rx_paused` and anchors the animation clock at the current time
  (`pause_anchor`); `expected_lines` and the intro scene use this frozen time, so the sheet
  and preview progress stand still. The panel shows "PAUSE" and a "Paused - waiting for sender"
  banner; the LOST timeout and the Verifying spinner are suppressed while paused.
- On RESUME(n) it clears `rx_paused` and sets `started_ms = now - (n - SSTV_ILEAVE) *
  SSTV_PHYS_US`, so the timeline maps to physical frame n immediately and the animation
  continues from where the picture was. Data integrity needs nothing from this: positions and
  the outer code are keyed on headers, not on time.

## Edge cases

- Pause requested on the last data frame is ignored (the pass finishes normally).
- If the sender stops while paused (Back again), no END is sent; the receiver stays on "Paused"
  until the viewer leaves with A/B. A pause-timeout auto-finish is a possible follow-up.
- Resume after a long silence relies on the 0.4 s leader plus the demod's per-tone gain
  trackers to re-settle before the first data frame.

## Not covered

Pausing the tone test; pausing a note pass (short enough that it is not needed); a maximum
pause duration.
