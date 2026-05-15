# CIDOO ABM066 Tools

This repo contains macOS helpers for investigating the CIDOO ABM066 screen and
clock HID protocol. The protocol and data-format notes live in
`CIDOO_ABM066_SCREEN_PROTOCOL.md`; this file is for tool usage and safety
workflows.

## Build

```sh
make -C tools
```

## Safety Gates

- Dry-run commands do not open HID and do not send reports.
- Template reads send command `0x05`; this is a non-mutating protocol query, but
  it is still a USB/HID output report.
- `cidoo-clock` no longer requires write confirmation flags. Clock updates still
  re-read the current record and verify that only bytes `35..41` change.
- `cidoo-image upload-buckets` no longer requires write confirmation flags; use
  `dry-run-buckets` first when changing inputs.
- Bucket image uploads are confirmed on ABM066 with the `lcd160x96` encoder.
  After command `0x23` is sent, a response timeout/status does not stop the
  image stream, matching the Windows worker. `--debug` prints packet dumps,
  hashes, config diffs, and command-level timing details.

## Known-Good Template Backup

Known-good restored template:

```text
backups/cidoo-template-known-good-restored-latest.bin
```

Known-good hashes:

```text
template sha256: b7f6a3ddbdf5412fe750612e445913a0a1ecfef875e9cbcbe82a8d6de3201e14
stable sha256:   7feb12ff4d6aafc9e0d95fec4c2d4ab366275cfdbdee397545a84230d580cbff
```

The stable hash masks clock bytes `35..41`, so it stays stable as time changes.

Important limit: this is a backup of the 48-byte config/template record only.
It is not a backup of the custom-image frame store. The image-store readback
protocol has not been identified, so the original bucket image bytes are not
backed up here.

## Clock Workflow

Read the current 48-byte template:

```sh
./tools/cidoo-clock read-template \
  --slot 0 \
  --out cidoo-template.bin
```

Dry-run a clock update from a saved template:

```sh
./tools/cidoo-clock dry-run \
  --template-file cidoo-template.bin \
  --debug
```

Write only the clock bytes:

```sh
./tools/cidoo-clock update-time
```

Restore the known-good 48-byte template:

```sh
./tools/cidoo-clock restore-template \
  --template-file backups/cidoo-template-known-good-restored-latest.bin
```

Optional hash guards are still available as `--expected-stable-sha256` for the
currently connected device and `--expected-template-sha256` for a restore file.
If the device currently matches the known-good stable hash, the value will be:

```text
7feb12ff4d6aafc9e0d95fec4c2d4ab366275cfdbdee397545a84230d580cbff
```

## Image Dry Runs

Portrait single-image packet dry-run:

```sh
./tools/cidoo-image dry-run \
  --image path/to/image.png \
  --preview-out /private/tmp/cidoo-physical-preview.png \
  --framebuffer-preview-out /private/tmp/cidoo-framebuffer-preview.png \
  --debug
```

Two-bucket dry-run with a freshly read template:

```sh
./tools/cidoo-image dry-run-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --template-file cidoo-template.bin \
  --debug
```

Expected ABM066 payload properties:

```text
physical preview: 96x160
rotation into framebuffer: not used
active selector byte after upload: 0
clock bytes: refreshed to local time
device layout: lcd160x96
device logical frame: 160x96
device bytes per frame: 32768
chunk bytes: 24
final GIF1 frame count: 1
final GIF2 frame count: 1
final total frame count: 2
payload bytes sent: 65536
image packet count: 2731
```

## Image Uploads

Real custom-image uploads must provide both buckets. The tool refuses one-bucket
uploads because the Windows app writes the combined custom-image store from
offset `0` and command `0x23` appears to prepare that whole store.

GIF support is still the riskiest host-side conversion difference. The Windows
app imports GIFs through OpenCV `VideoCapture`, then resizes/rotates each decoded
video frame. That resize is stretch-to-fit, not aspect-preserving letterbox or
cover. Community ABM066 notes identify the screen as `96x160`, a `3:5` portrait
ratio. The tool now defaults to a native `96x160` source canvas, rotates it into
`160x96` RGB565 device memory, and pads each frame to the `0x8000` byte frame
stride. GIFs are
automatically coalesced to full PNG frames with ImageMagick before encoding so
partial GIF frame rectangles are not treated as complete frames. Inspect the
bucket previews before any real GIF upload.

Current upload command shape:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif
```

The bucket uploader re-reads the current config record immediately before
writing, preserves the current selector byte at offset `33` by default,
verifies that only expected config offsets changed, writes the 48-byte config,
then reads the config back and checks the stable hash before any image data is
sent. With `--debug`, it also prints hashes, config diffs, HID packets, and the
Windows-derived command `0x23` prepare timeout formula. The local uploader waits
only the normal HID timeout for a response; if `0x23` does not respond, it still
waits the remaining Windows-derived prepare time before streaming command `0x21`
packets. Starting `0x21` immediately after a `0x23` timeout can make the first
image packet time out while the keyboard is still preparing/erasing the image
store.

Command `0x21` image data now defaults to the Windows constructor's `24` byte
chunk mode. Static analysis also found a `56` byte mode, but live ABM066 tests
with `56` byte image chunks produced regularly cut/interlaced output.

Live mapping tests showed that the device indexes custom image frames at
`32768` byte boundaries, not at the host-side `65536` byte padded frame boundary
from the Windows all-frame buffer. The uploader now places bucket frames on
`32768` byte boundaries by default. The current encoder writes the `96x160`
physical source as `160x96` RGB565 memory (`30720` bytes), followed by `2048`
padding bytes. Writing the `96x160` source row-major produced source top/bottom
halves on opposite screen sides and spread corner markers through the height,
which is consistent with the device reading landscape memory and rotating it
onto the portrait LCD.

`--expected-stable-sha256 HEX` is still available as an optional extra guard,
but normal uploads do not need it.

### Real Upload Preflight

Before any real image upload:

```sh
make -C tools

./tools/cidoo-clock read-template \
  --slot 0 \
  --out cidoo-template.bin

./tools/cidoo-image dry-run-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --template-file cidoo-template.bin \
  --debug
```

Check the dry-run output before writing:

- It ends with `No HID device was opened. No report was sent.`
- `payload start byte offset` is `0`.
- `cmd 0x23 image prepare frame basis` is the larger of the current and final
  total frame counts.
- `cmd 0x23 Windows prepare timeout formula` is `frame_basis * 300ms + 500ms`;
  the actual response wait, `cmd 0x23 local prepare wait`, defaults to
  `1500 ms`. If no response arrives, the uploader sleeps the remaining Windows
  formula time before the first `0x21` image packet.
- `active selector byte after upload` preserves the current template value; on
  the known-good restored template this is `0`.
- Changed config offsets are only `34`, `46`, plus clock bytes `35..41` unless
  `--preserve-clock` is used. Offset `33` should only change if using the
  diagnostic `--active-bucket` override.

For two 8-frame GIFs against the known-good `20 + 8` template, the final total
is 16 frames and the prepare basis is 28 frames. The Windows formula would be
`8900 ms`, but the local `0x23` wait remains the normal HID timeout.

Real upload, after the preflight checks:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif
```

### GIF Conversion Notes

OpenCV is not required for protocol reliability. The keyboard does not receive
OpenCV data; it receives RGB565 frame bytes. The important protocol properties
are the 48-byte metadata record, combined GIF1+GIF2 frame ordering, command
`0x23`, and the command `0x21` RGB565 stream.

The Windows app uses OpenCV `VideoCapture` for GIF/video import, so the only
OpenCV-relevant risk is visual frame decoding. The uploader uses ImageMagick's
`magick input.gif -coalesce PNG32:...` path for GIFs. If `magick` is unavailable,
GIF uploads fail before any HID write is sent.

If a GIF looks garbled on-device but a recognizable outline is visible, test one
still frame in both buckets before changing more protocol code. This separates
scan/transport problems from animation tearing or GIF-frame compositing:

```sh
magick input.gif -coalesce -delete 1--1 PNG32:/private/tmp/cidoo-first-frame.png

./tools/cidoo-image upload-buckets \
  --bucket1 /private/tmp/cidoo-first-frame.png \
  --bucket2 /private/tmp/cidoo-first-frame.png
```

A clean still image means RGB565 encoding, packet order, and the basic image
store write are probably correct; the remaining issue is multi-frame layout or
animation timing/compositing. A garbled still image means the bug is lower-level
than GIF animation.

For row/column scan diagnostics, use one-frame stripe images in the two buckets:

```sh
magick -size 96x160 xc:white -fx 'mod(j,2)' /private/tmp/cidoo-row-stripes.png
magick -size 96x160 xc:white -fx 'mod(i,2)' /private/tmp/cidoo-column-stripes.png

./tools/cidoo-image upload-buckets \
  --bucket1 /private/tmp/cidoo-row-stripes.png \
  --bucket2 /private/tmp/cidoo-column-stripes.png
```

Bucket 1 should show horizontal one-pixel stripes; bucket 2 should show vertical
one-pixel stripes. If those are clean, the host payload order is not the source
of the banana artifact. If both custom views show horizontal stripes, or if the
same image appears split between left and right halves, first check that the
upload printed `active selector byte after upload: 0` and that changed config
offsets did not include `33`.

### Previous Failure Theory

The earlier broken image uploads are most consistent with two protocol mistakes:

- A partial/one-bucket upload is unsafe because command `0x23` appears to act on
  the combined custom-image store, and the Windows sender always starts command
  `0x21` at byte offset `0`.
- The old uploader treated a command `0x23` timeout as fatal after the config
  metadata had already been changed. That can leave the keyboard pointing at new
  frame counts or buckets while no matching frame stream was programmed.
- An older uploader also forced config offset `33` to `1`. The known-good
  restored template has offset `33 = 0`, and forcing it away from `0` correlated
  with split/striped custom-image output during live tests.

The current uploader still requires both buckets and now follows the Windows
worker by continuing to stream command `0x21` packets after command `0x23` is
sent, even if command `0x23` does not produce a normal response.

### Recovery Notes

If the screen is wrong after an experiment, first read the current config record
and use the stable hash printed by that read:

```sh
./tools/cidoo-clock read-template \
  --slot 0 \
  --out cidoo-template.bin
```

Then restore the known-good 48-byte config template:

```sh
./tools/cidoo-clock restore-template \
  --template-file backups/cidoo-template-known-good-restored-latest.bin
```

This restores config metadata only. If the image store itself has been erased or
partially programmed, the metadata restore may not bring back the original image
bytes.

## Current Investigation Status

Confirmed:

- Config writes patch a 48-byte template; image bucket metadata is offsets
  `33`, `34`, and `46`. The current uploader preserves offset `33` by default
  because the exact selector semantics are not fully resolved and the known-good
  ABM066 template uses `0`.
- GIF1 and GIF2 share one combined custom-image store: GIF1 frames first, GIF2
  frames second, command `0x21` byte offset starts at `0`.
- Both confirmed Windows packers write their source `240x135` frames in ordinary
  row-major order: `payload[(y * width + x) * 2]`. No odd/even row interleave,
  column-major order, or serpentine scan mapping has been found in the Windows
  host-side frame builder.
- Live ABM066 custom-view tests indicate the display consumes `32768` byte
  device frames, not the Windows host-side `65536` byte frame stride. Community
  notes identify the actual screen resolution as `96x160`. The current candidate
  is a `96x160` source rotated into `160x96` memory:
  `160 * 96 * 2 = 30720` bytes plus `2048` bytes of frame padding. The previous
  `96x160` row-major, `135x120`, and `128x128` candidates produced
  staircase/interleave artifacts or had the wrong aspect ratio.
- The Windows command `0x23` wrapper uses timeout `total_frames * 300ms + 500ms`.
  The local uploader waits only the normal HID timeout for a response, then
  sleeps the remaining Windows-derived prepare time if no response arrived,
  because the first `0x21` packet can time out while the device is still
  preparing/erasing the image store.
- Animation interval is stored globally at config offsets `43..44`; the default
  template value is `100` ms. Community notes recommend exporting GIFs at
  `96x160`, `16` fps and setting the interval to `60` ms for smooth playback.
- OpenCV is not part of the device protocol; it affects host-side GIF/video
  frame decoding and stretch-to-fit resizing before RGB565 conversion.

Still unresolved:

- The exact erase/prepare behavior behind command `0x23`.
- Whether the keyboard firmware always replies to command `0x23`.
- Exact OpenCV GIF compositing behavior for partial-frame GIFs.
- Any protocol command for reading back the custom-image frame store.
