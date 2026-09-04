# SFTV notes: send text as a note, not an image

Date: 2026-09-04
Status: approved design
Base: `2026-09-03-sstv-outer-code-design.md` (revision 7 protocol), `2026-09-03-sftv-web-design.md`

## Goal

Send a short text note from the Flipper to the Playdate over the same channel as an image.
The note travels through the same modem, framing, inner and outer RS code, and the same
"sheet slides in from the edge" animation, but its content is ASCII text, not a 1-bit raster.
The Playdate renders the text itself (no rasterising on the Flipper) and keeps notes as a
separate media type in the gallery, drawn as a sticky note with a border.

## Decisions (from discussion)

- **Charset:** ASCII only (Latin + digits + punctuation). No Cyrillic in v1, so the Momentum
  keyboard and the current Playdate font work unchanged.
- **Length:** short, one screen. Cap 200 characters = 8 text frames.
- **Composing on the Flipper:** the Momentum on-screen keyboard (`text_input`), plus 5-7
  bundled preset notes you can pick and edit before sending.
- **On the Playdate:** notes are a first-class media type. Received notes are saved to the
  gallery and shown as a styled sticky note (paper rectangle, border, folded corner); the
  sticky-note look is a client-side template rendered on the Playdate.
- **Wire:** one new flag bit in the START meta marks a note pass; otherwise the existing
  frame format, interleave, inner RS and outer RS parity are reused as-is.
- **Rejected alternative** (the user rejected it explicitly): rasterise the text into a
  400x240 1-bit image on the Flipper and send it as an ordinary image. Zero protocol and
  Playdate change, but defeats the point (Flipper renders, no text semantics on receipt).

## Wire format (revision 8, backward compatible)

A note pass is a normal pass with a new content flag and no image-preview stages.

- **START / END meta** carry the total physical frame count in a 16-bit word. Bit 15 is the
  existing `SSTV_META_CODED_FLAG`. Add `SSTV_META_NOTE_FLAG 0x4000` (bit 14). A note pass sets
  bit 14 and never bit 15 (notes are never coded). Bits 0..13 hold the total (<= 16383, ample).
  The meta `name` field (23 bytes) holds a short label for the gallery: the note's first line
  truncated to 23 chars (or "Note" if empty).
- **No tiny / preview / schedule.** A note has nothing to preview.
- **Text frames:** ordinary `SSTV_FT_RAW` frames whose 26-byte payload is ASCII text (only the
  first `SSTV_SEG_BYTES` = 25 bytes are used, matching an image half-line; the 26th stays 0),
  header index = chunk number 0..K-1. The last chunk is zero-padded. K = ceil(len / 25),
  padded up to a multiple of `SSTV_ILEAVE` with empty (all-zero) text frames. ASCII text has
  no NUL, so the receiver concatenates all chunk payloads and trims trailing NULs.
- **Layout / grouping:** the whole note is one outer-code group (group 0). `intro` (group-0
  size) = `SSTV_ILEAVE` (START) + K text frames = the note's data-frame count. So a note has
  exactly one group; its 12 parity frames follow, then END. Positions: START `i` at 0..3, text
  chunk `c` at `SSTV_ILEAVE + c`. `total = data_frames + SSTV_OUTER_PARITY + SSTV_ILEAVE`
  (data + one parity block + END).
- **Cap:** 200 chars -> K = 8 -> data_frames = 12, total = 12 + 12 + 4 = 28 physical frames,
  ~7.5 s including the 2 s leader. Room protection is strong (12 parity for 8 text frames).

Constants (`common/sstv_params.h`): `SSTV_META_NOTE_FLAG 0x4000`, `SSTV_NOTE_MAX_CHARS 200`,
`SSTV_NOTE_CHUNK SSTV_SEG_BYTES` (25), `SSTV_NOTE_MAX_FRAMES` = ceil(200/25) rounded up to
`SSTV_ILEAVE` (8).

## Protocol (`common/`)

- **`sstv_build_note(const char* text, const char* name, uint8_t* out, uint8_t* lines_after)`**
  in `sstv_frame.c/.h`: builds START (NOTE flag), then K text frames as RAW frames feeding the
  26 streaming RS encoders, closes the single group with its parity block, then END. Returns
  the physical frame count. `lines_after[phys]` = text chunks complete after that physical
  frame (for the Flipper progress display; parity/END repeat the last value). Mirrors
  `sstv_build_physical` but with the note layout.
- **Layout helpers:** `sstv_pass_position` / `sstv_pass_meta_position` already take the header
  and compute image positions; the note builder computes positions directly (START `i`, text
  `SSTV_ILEAVE + index`) rather than overloading the image helpers. `sstv_pass_total`,
  `sstv_pass_groups`, `sstv_pass_group_*`, and the parity emitter take `intro` as a plain int,
  so they are reused unchanged with `intro = SSTV_ILEAVE + K`.
- **Receiver (`sstv_rx.c/.h`):**
  - New state `bool note` (set from the START flag, like `coded`).
  - `data_frames_from_total` branches: for a note, `data_frames = total - SSTV_OUTER_PARITY -
    SSTV_ILEAVE`, `intro = data_frames` (single group).
  - `meta_cb` gains a `bool note` argument: `(ctx, name, is_end, total, coded, note)`.
  - New `sstv_note_cb(void* ctx, int chunk, int nbytes, const uint8_t* text)` called from
    `handle_frame` when a note pass delivers a RAW text frame (and again after an outer-code
    repair). The Playdate stores chunks at `chunk * 25` and re-renders. On END the accumulated
    buffer (trailing NULs trimmed) is the final note.
  - `sstv_rx_init` gains the note callback. In a note pass, RAW frames do NOT go to `pixel_cb`.
- **Header of a repaired note frame:** rebuilt from its position exactly like a raw frame
  (`SSTV_FT_RAW`, index = position - `SSTV_ILEAVE`); no CRC needed after repair.

## Flipper (`flipper/`)

- **New top menu** (a `Submenu` view or a two-tile custom view): **Send image** (the new
  animated `A_scan` icon moves here) opens the existing file browser; **Send note** opens the
  note flow. The app now starts on this menu instead of the browser.
- **Browser header reverted:** `draw_browse` goes back to the static "TX" pill + "Slow Flipper
  TV" title it had before the animated icon was placed there. The animated icon is now the
  Send-image tile.
- **Note flow:** a list of the bundled presets plus "New note...". Selecting either opens the
  Momentum `text_input` (ASCII, max `SSTV_NOTE_MAX_CHARS`) preloaded with the preset text (or
  empty). On accept, a preview screen shows the text and sends on OK via a new
  `sstv_tx_start_note(const char* text, SstvTxCallback, void* ctx)` in `flipper/sstv_tx.c`,
  which calls `sstv_build_note` into the same `tx.frames` buffer and drives the same modem/ISR
  path. Progress, LED, and the sending sheet are shared with images. `name` = first line of
  the note truncated to `SSTV_NAME_MAX`.
- **Presets** (`flipper/sstv_notes_presets.h`, a static array of 5-7 short ASCII strings):
  a greeting, a "hello from Flipper", a text test pattern (`ABCDEFG... 0123456789`), a longer
  paragraph near the 200-char cap (to exercise wrapping and scroll edge), a smiley/ASCII-art
  line, a "meet me at ..." note, a Playdate in-joke. All <= 200 chars, ASCII.

## Playdate (`playdate/`)

- **Reception:** the intro scene plays as for an image (dolphin reacts to tone). Since a note
  has no image preview, the console shows a document placeholder instead of the filling
  preview. Then a **sticky note** slides up from the bottom edge like the image sheet, and the
  text fills in as frames arrive (each text frame reveals ~25 chars). One screen, no scroll.
  The title bar reads `Receiving: <name>` as today.
- **Sticky-note renderer** (client template, in `playdate/src/`): a light paper rectangle with
  a 1px border and a folded top-right corner, inner margin, the note text word-wrapped in the
  UI font, left-aligned. Used both live during reception and in the gallery. A single
  `draw_sticky_note(rect, text)` function is the template.
- **Gallery as a media type:** the gallery model gains an item type (image vs note). Notes are
  saved as text files (e.g. `notes/<timestamp>.txt` with the label as the first line);
  thumbnails render the sticky-note template scaled down. Opening a note shows the full sticky
  note (with the same delete-in-full-view + auto-hiding hints already built for images). Image
  items are unchanged.
- **Wire-up:** `main.c` registers the new `sstv_note_cb`, accumulates the text buffer, sets
  `sstv_rx.note`, and on END writes the note file and adds it to the gallery. `meta_cb` reads
  the new `note` argument to switch the reception scene into note mode.

## Tests

- `test_frame`: a built note pass has START (NOTE flag), K text frames at the right positions,
  one parity block, END last; total matches `SSTV_NOTE_*`; positions round-trip.
- `test_modem` (loopback): modulate a note, demodulate -> the text comes back byte-for-byte;
  drop up to 12 frames of the group -> still recovered (`frames_repaired` matches); a note with
  the max 200 chars round-trips; an empty/short note round-trips.
- `sstv_decode_wav` prints the decoded note text when the NOTE flag is set.

## Out of scope (follow-ups)

- **Web emulator note support:** the JS port would need the same note branch to send/receive
  notes browser<->hardware. Not in v1 (the request is Flipper -> Playdate); the wire format is
  designed so the JS port can add it later without a protocol change.
- Cyrillic / full UTF-8 notes (keyboard layout + font).
- Long, scrolling notes (multi-screen sticky notes).
- Editing or replying to a received note on the Playdate.
