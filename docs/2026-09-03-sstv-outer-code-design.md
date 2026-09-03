# SFTV revision 7: outer Reed-Solomon code across frames

Date: 2026-09-03
Status: approved design
Base: `2026-09-03-sstv-flipper-playdate-design.md`, "Current protocol (revision 6)"

## Problem

Measured on Playdate recordings (`recordings/2026-09-03-distance/`): tones never confuse each
other, but the tone-or-silence decision breaks with distance because a room echo (~7 ms, −9 dB
at 3 cm, −4 dB at 20 cm) lands in the decision window two symbols later. Adaptive thresholds,
energy-domain leak subtraction and coherent echo cancellation were all tried on the same
recordings and rejected; dropping the silence symbol (4-FSK on 2500/3000/3500/4000) would
help but the user wants white areas to stay quiet. So the physical layer stays as it is and
the frame layer gets an erasure code across frames: any 12 lost frames per group of 108 are
rebuilt, whatever type they are. Coded (compressed) passes, where one lost frame currently
costs the rest of a block, become as robust as raw passes.

## Layout of a pass

Logical frames are numbered by *data position*, exactly the order revision 6 sends them,
minus END:

| position | frames |
|---|---|
| 0..3 | START block (identical frames) |
| 4..7 | TINY preview chunks 0..3 |
| 8..27 | PREVIEW chunks 0..19 |
| 28..31 | SCHEDULE chunks 0..3 (coded passes only) |
| then | data: RAW segment s at 28 + s; CODED block b, frame i at (b + 1) × 4 + i, where b is the header index (data block number − 1, as today: the first coded block has index 7 and sits at 32) |

Positions are grouped: group g = positions 108g .. 108g + 107 (27 blocks, ~73 s). After the
last block of each group the transmitter inserts 3 parity blocks = 12 frames of type
`PARITY` (3, the free header type), index = g × 12 + j. The last group is shorter (shortened
RS codeword). END (4 identical META frames) is sent after the last parity block and is not
covered, so the receiver finishes only after the last repair opportunity.

Parity: for each byte column k of the 29 data bytes of a logical frame (header, payload,
CRC), the codeword is [d_0[k] … d_{n−1}[k] | p_0[k] … p_11[k]] with the existing RS(…, 12)
code over GF(256) (`rs.c`, same generator as the inner code). Parity frame j carries p_j[k]
in its 29 data bytes and is itself packed like any logical frame (inner RS parity added on
top). The pass total announced in START/END counts parity frames; on a raw photo the pass
grows from 508 to 568 frames (+11 %, 5.4 → 6.0 min).

Constants (`sstv_params.h`): `SSTV_FT_PARITY 3`, `SSTV_OUTER_GROUP 108`,
`SSTV_OUTER_PARITY SSTV_PARITY_BYTES` (12), `SSTV_OUTER_GROUPS_MAX` derived from
`SSTV_MAX_FRAMES`. `SSTV_MAX_FRAMES` grows by the parity frames.

Layout helpers in `common/sstv_frame.h` shared by transmitter, receiver and Playdate
timeline:
- `sstv_pass_position(hdr, i)` → data position (or −1 for END/PARITY), using the table above;
- `sstv_pass_groups(data_frames)`, `sstv_pass_total(data_frames)`;
- `sstv_pass_data_before(phys_index, data_frames)`: data frames sent once `phys_index`
  physical frames are out, for the Playdate timeline and the Flipper progress display.

## Transmitter (`sstv_build_physical`)

Builds frames in data order as today, feeding every logical frame's 29 data bytes into 29
streaming RS encoders (`rs_encoder_t` state of 12 bytes: `rs_encoder_init`, `rs_encoder_feed(byte)`,
`rs_encoder_finish(parity)`, added to `rs.c`; `rs_encode` becomes a wrapper). After each 108
frames, and after the last data frame, it emits the 12 parity frames as 3 interleaved blocks
and resets the encoders. END goes last. `lines_after` for parity and END frames repeats the
previous value. The Flipper LED thread shows parity frames in a distinct colour (green at the
tone's brightness instead of blue); `sstv_tx_current_symbol()` gains a companion
`sstv_tx_current_is_parity()` (or the symbol callback carries a flag) so the LED thread knows.

## Receiver (`common/sstv_rx`)

New state: a pass buffer `frames[SSTV_MAX_DATA_FRAMES][29]` with a presence bitmap, parity
buffer `parity[SSTV_OUTER_GROUPS_MAX][12][29]` with presence bits, and the `coded` flag (from
START or the first data frame). About 15 kB.

`try_block` keeps its physical → logical decoding; the per-frame dispatch (pixels, meta,
preview) moves into `handle_frame(rx, hdr, payload, from_repair)`. Every decoded logical frame
is also stored at its data position (or parity slot). Coded blocks: the block is unpacked
from the stored payloads of its 4 positions, up to the first missing one (today's behaviour),
and unpacked again in full after a repair completes the block.

Repair runs when a parity frame arrives and its group has missing data frames (or missing
parity) with erasures ≤ 12: for each of the 29 columns build the codeword from stored bytes
(missing frames = erasure positions), `rs_decode_erasures`, write the recovered bytes back.
Each recovered frame is CRC-checked, then dispatched through `handle_frame` (pixels fill in,
previews complete, coded blocks re-unpack). Counters: `frames_repaired`, `groups_repaired`,
`repairs_failed`. A group that never got enough frames stays as is.

`sstv_rx_init` / START reset the buffers. The receiver must not depend on physical frame
counting: positions come from headers only, so flywheel slips or lost syncs cannot misplace
a frame.

## Playdate

`expected_lines` (timeline) maps elapsed physical frames to data frames with
`sstv_pass_data_before`, so parity blocks do not advance the sheet. Panel: a "fixed N"
counter (`frames_repaired`). Nothing else changes: late lines already overwrite the
placeholder through `on_pixels`; END arrives after the last repair, so the saved file
includes repairs.

## Tests

- `test_rs`: streaming encoder equals `rs_encode`; erasure decoding of a shortened codeword.
- `test_frame`: layout helpers (positions round-trip for raw and coded passes, totals, data
  frames before a physical index); a built pass has parity blocks in the right places and END
  last.
- `test_modem` (loopback): drop any 12 physical frames of a group (including whole blocks and
  parity frames) → 240/240 lines and previews complete, `frames_repaired` matches; drop 13 →
  the group stays short and the rest of the pass is unaffected; coded pass with a lost middle
  frame of a block recovers the whole block after the group's parity.
- `sstv_decode_wav` prints `repaired=`.
- Hardware: coded checker and a raw photo at 10 cm, compare line counts and the "fixed"
  counter with revision 6 recordings.

## Out of scope (follow-ups)

Record offset table inside coded blocks (only matters beyond 12 losses per group); reducing
the inner RS parity now that the outer code exists (needs measurements); the tone test mode
and the Playdate `Down` test recording stay in until revision 7 is verified, then go.
