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

## Current Investigation Status

Confirmed:

- Config writes patch a 48-byte template; image bucket metadata is offsets
  `33`, `34`, and `46`.
- GIF1 and GIF2 share one combined custom-image store: GIF1 frames first, GIF2
  frames second, command `0x21` byte offset starts at `0`.
- Command `0x23` uses timeout `total_frames * 300ms + 500ms`.
- Animation interval is stored globally at config offsets `43..44`; the default
  is `100` ms.

Still unresolved:

- The exact erase/prepare behavior behind command `0x23`.
- Whether the keyboard firmware always replies to command `0x23`.
- Exact OpenCV GIF compositing behavior for partial-frame GIFs.
