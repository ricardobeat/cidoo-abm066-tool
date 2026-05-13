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
- Config writes require explicit write flags and a confirmation string.
- Image writes require explicit write flags and `--confirm-upload
  UPLOAD_SCREEN_IMAGE`.
- Bucket image uploads are still experimental. The current implementation
  matches the Windows-app command order and the Windows-derived command `0x23`
  timeout. After command `0x23` is sent, a response timeout/status does not stop
  the image stream, matching the Windows worker. There is not yet a confirmed
  successful real image upload.

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
  --allow-hid-query \
  --slot 0 \
  --out cidoo-template.bin
```

Dry-run a clock update from a saved template:

```sh
./tools/cidoo-clock dry-run \
  --template-file cidoo-template.bin \
  --print-packets
```

Write only the clock bytes after checking the current stable hash:

```sh
./tools/cidoo-clock update-time \
  --allow-hid-query \
  --allow-config-write \
  --expected-stable-sha256 PUT_CURRENT_STABLE_SHA256_HERE \
  --confirm-write PATCH_ONLY_BYTES_35_41
```

Restore the known-good 48-byte template after first reading the current stable
hash:

```sh
./tools/cidoo-clock restore-template \
  --allow-hid-query \
  --allow-config-write \
  --template-file backups/cidoo-template-known-good-restored-latest.bin \
  --expected-stable-sha256 PUT_CURRENT_STABLE_SHA256_HERE \
  --expected-template-sha256 b7f6a3ddbdf5412fe750612e445913a0a1ecfef875e9cbcbe82a8d6de3201e14 \
  --confirm-write RESTORE_48_BYTE_TEMPLATE
```

Use the current stable hash from a fresh `read-template`, not an old hash copied
from this file. If the device currently matches the known-good stable hash, the
value will be:

```text
7feb12ff4d6aafc9e0d95fec4c2d4ab366275cfdbdee397545a84230d580cbff
```

## Image Dry Runs

Portrait single-image packet dry-run:

```sh
./tools/cidoo-image dry-run \
  --image path/to/image.png \
  --orientation portrait \
  --fit cover \
  --preview-out /private/tmp/cidoo-physical-preview.png \
  --framebuffer-preview-out /private/tmp/cidoo-framebuffer-preview.png \
  --print-packets
```

Two-bucket dry-run with a freshly read template:

```sh
./tools/cidoo-image dry-run-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --template-file cidoo-template.bin \
  --orientation portrait \
  --print-packets
```

Expected ABM066 payload properties:

```text
physical preview: 135x240
rotation into framebuffer: 90
framebuffer: 240x135
rgb565 bytes per frame: 64800
padded bytes per frame: 65536
chunk bytes: 56
image packet count per frame: 1171
```

## Image Uploads

Real custom-image uploads must provide both buckets. The tool refuses one-bucket
uploads because the Windows app writes the combined custom-image store from
offset `0` and command `0x23` appears to prepare that whole store.

GIF support is still the riskiest host-side conversion difference. The Windows
app imports GIFs through OpenCV `VideoCapture`, then resizes/rotates each decoded
video frame. This tool currently decodes frames with macOS ImageIO. Inspect the
bucket previews before any real GIF upload, especially for GIFs whose frames are
subrectangles rather than full-canvas frames.

Current upload command shape:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --orientation portrait \
  --allow-hid-query \
  --allow-config-write \
  --allow-image-write \
  --expected-stable-sha256 PUT_CURRENT_STABLE_SHA256_HERE \
  --confirm-upload UPLOAD_SCREEN_IMAGE \
  --print-packets
```

The bucket uploader re-reads the current config record, verifies the current
stable hash, verifies that only expected config offsets changed, uses the
Windows-derived command `0x23` prepare timeout
`total_frames * 300ms + 500ms`, then sends the combined frame stream. If command
`0x23` times out after the report is sent, the uploader continues with command
`0x21` packets because the Windows all-frame worker does the same.

### Real Upload Preflight

Before any real image upload:

```sh
make -C tools

./tools/cidoo-clock read-template \
  --allow-hid-query \
  --slot 0 \
  --out cidoo-template.bin

./tools/cidoo-image dry-run-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --template-file cidoo-template.bin \
  --orientation portrait \
  --print-packets
```

Check the dry-run output before writing:

- It ends with `No HID device was opened. No report was sent.`
- `payload start byte offset` is `0`.
- `cmd 0x23 image prepare timeout` is `total_frames * 300ms + 500ms`.
- Changed config offsets are only `33`, `34`, `46`, plus clock bytes `35..41`
  unless `--preserve-clock` is used.
- `template stable sha256` is the hash to pass as
  `--expected-stable-sha256` for the real upload.

For two 8-frame GIFs, the expected total is 16 frames and the command `0x23`
timeout is `5300 ms`.

Real upload, after the preflight checks:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --orientation portrait \
  --allow-hid-query \
  --allow-config-write \
  --allow-image-write \
  --expected-stable-sha256 PUT_CURRENT_STABLE_SHA256_HERE \
  --confirm-upload UPLOAD_SCREEN_IMAGE \
  --print-packets
```

### GIF Conversion Notes

OpenCV is not required for protocol reliability. The keyboard does not receive
OpenCV data; it receives RGB565 frame bytes. The important protocol properties
are the 48-byte metadata record, combined GIF1+GIF2 frame ordering, command
`0x23`, and the command `0x21` RGB565 stream.

The Windows app uses OpenCV `VideoCapture` for GIF/video import, so the only
OpenCV-relevant risk is visual frame decoding. For suspicious GIFs, compare the
payload hash from the original GIF with a coalesced copy:

```sh
magick input.gif -coalesce /private/tmp/cidoo-coalesced.gif

./tools/cidoo-image dry-run-buckets \
  --bucket1 input.gif \
  --bucket2 input.gif \
  --template-file cidoo-template.bin \
  --orientation portrait

./tools/cidoo-image dry-run-buckets \
  --bucket1 /private/tmp/cidoo-coalesced.gif \
  --bucket2 /private/tmp/cidoo-coalesced.gif \
  --template-file cidoo-template.bin \
  --orientation portrait
```

If the `payload sha256` values match, GIF coalescing is not changing what the
uploader will send for that file. `~/Downloads/banana.gif` was checked this way:
the original and an ImageMagick `-coalesce` copy produced the same payload hash.

### Previous Failure Theory

The earlier broken image uploads are most consistent with two protocol mistakes:

- A partial/one-bucket upload is unsafe because command `0x23` appears to act on
  the combined custom-image store, and the Windows sender always starts command
  `0x21` at byte offset `0`.
- The old uploader treated a command `0x23` timeout as fatal after the config
  metadata had already been changed. That can leave the keyboard pointing at new
  frame counts or buckets while no matching frame stream was programmed.

The current uploader still requires both buckets and now follows the Windows
worker by continuing to stream command `0x21` packets after command `0x23` is
sent, even if command `0x23` does not produce a normal response.

### Recovery Notes

If the screen is wrong after an experiment, first read the current config record
and use the stable hash printed by that read:

```sh
./tools/cidoo-clock read-template \
  --allow-hid-query \
  --slot 0 \
  --out cidoo-template.bin
```

Then restore the known-good 48-byte config template:

```sh
./tools/cidoo-clock restore-template \
  --allow-hid-query \
  --allow-config-write \
  --template-file backups/cidoo-template-known-good-restored-latest.bin \
  --expected-stable-sha256 PUT_CURRENT_STABLE_SHA256_HERE \
  --expected-template-sha256 b7f6a3ddbdf5412fe750612e445913a0a1ecfef875e9cbcbe82a8d6de3201e14 \
  --confirm-write RESTORE_48_BYTE_TEMPLATE
```

This restores config metadata only. If the image store itself has been erased or
partially programmed, the metadata restore may not bring back the original image
bytes.

## Current Investigation Status

Confirmed:

- Config writes patch a 48-byte template; image bucket metadata is offsets
  `33`, `34`, and `46`.
- GIF1 and GIF2 share one combined custom-image store: GIF1 frames first, GIF2
  frames second, command `0x21` byte offset starts at `0`.
- Command `0x23` uses timeout `total_frames * 300ms + 500ms`.
- Animation interval is stored globally at config offsets `43..44`; the default
  is `100` ms.
- OpenCV is not part of the device protocol; it only affects host-side GIF/video
  frame decoding before RGB565 conversion.

Still unresolved:

- The exact erase/prepare behavior behind command `0x23`.
- Whether the keyboard firmware always replies to command `0x23`.
- Exact OpenCV GIF compositing behavior for partial-frame GIFs.
- Any protocol command for reading back the custom-image frame store.
