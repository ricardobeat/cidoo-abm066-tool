# CIDOO ABM066 Screen Mapping Test Images

Older images in this folder were generated at `135x240` while the screen mapping
was still unknown. The current working model is a native `96x160` physical
screen. `tools/cidoo-image` now scales inputs to `96x160`, rotates that source
into `160x96` device memory, then RGB565-encodes the memory frame.

## Files

- `cidoo-anchor-map.png`: large asymmetric corner/center markers. Use first if
  orientation or cropping is unclear.
- `cidoo-grid-a.png`: labeled 3x4 color block map for GIF1 / bucket 1.
- `cidoo-grid-b.png`: labeled 3x4 color block map for GIF2 / bucket 2.
- `cidoo-tile-5x8.png`: dense unlabeled 5x8 color tile map for finer mapping.
- `cidoo-grid-ab-2frame.gif`: two-frame GIF made from grid A then grid B.
- `cidoo-grid-a-prewrap-left45.png`: grid A with physical columns pre-wrapped
  left by 45 px. This should look normal on-device if the observed split is
  exactly a 45 px horizontal wrap.
- `cidoo-grid-b-prewrap-left45.png`: same compensation for grid B.
- `cidoo-x-ruler-9.png`: nine 15 px vertical bands, useful for measuring the
  wrap offset precisely.
- `cidoo-page-a-red-green.png`: bucket 1 page-boundary test, red `A0` on the
  left half and green `A1` on the right half.
- `cidoo-page-b-blue-yellow.png`: bucket 2 page-boundary test, blue `B0` on the
  left half and yellow `B1` on the right half.
- `cidoo-native96-bucket-a.png`: native `96x160` bucket 1 quadrant test.
- `cidoo-native96-bucket-b.png`: native `96x160` bucket 2 quadrant test.
- `cidoo-native96-x-ruler.png`: native `96x160` vertical color-band ruler.
- `cidoo-native96-y-ruler.png`: native `96x160` horizontal color-band ruler.
- `cidoo-native96-row-stripes-2px.png`: native `96x160` two-pixel horizontal
  stripe test.
- `cidoo-native96-column-stripes-2px.png`: native `96x160` two-pixel vertical
  stripe test.
- `cidoo-native96-ab-2frame.gif`: native `96x160` two-frame GIF using the A/B
  quadrant frames at `60` ms per frame.
- `cidoo-native96-lokaalhost22-scroll.gif`: native `96x160`, 64-frame,
  60 ms/frame monospace right-to-left scroll reading `lokaalhost:22`.
- `cidoo-grid-a-framebuffer.png`: dry-run preview of grid A after host rotation.
- `cidoo-grid-b-framebuffer.png`: dry-run preview of grid B after host rotation.
- `cidoo-test-contact-sheet.png`: reference sheet for quick visual comparison.
- `cidoo-wrap-test-contact-sheet.png`: reference sheet for the prewrap and
  ruler tests.
- `cidoo-page-test-contact-sheet.png`: reference sheet for the page-boundary
  test.

## Still Bucket Test

Use the native `96x160` files first. These avoid the older `135x240` test image
scaling step:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-native96-bucket-a.png \
  --bucket2 test-images/cidoo-native96-bucket-b.png \
```

Expected upload summary:

```text
active selector byte after upload: 0
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

If that still interlaces, test the native rulers next:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-native96-x-ruler.png \
  --bucket2 test-images/cidoo-native96-y-ruler.png \
```

The older grid files below are retained for historical comparison.

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-grid-a.png \
  --bucket2 test-images/cidoo-grid-b.png \
```

Expected upload summary:

```text
active selector byte after upload: 0
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

If bucket boundaries are correct, custom view 1 should show content from grid A
and custom view 2 should show content from grid B. The current default encoder
uses a native `96x160` source rotated into `160x96` device memory. Each frame is
`30720` RGB565 bytes plus `2048` padding bytes inside the `0x8000` byte device
frame.

## 45 px Wrap Compensation Test

The first grid test showed the image as a 45 px strip plus a 90 px strip, which
looks like a horizontal wrap by one third of the `135` px portrait width. This
test pre-wraps the input in the opposite direction.

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-grid-a-prewrap-left45.png \
  --bucket2 test-images/cidoo-grid-b-prewrap-left45.png \
```

If this compensation is right, the device should show the original unshifted
grid order: bucket 1 should read `A1 A2 A3` across the top row, and bucket 2
should read `B1 B2 B3` across the top row.

## X Ruler Test

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-x-ruler-9.png \
  --bucket2 test-images/cidoo-x-ruler-9.png \
```

Report the visible left-to-right band order. Unshifted order is
`0 1 2 3 4 5 6 7 8`.

## Page Boundary / Bucket Boundary Test

Use this after the ruler test. It checks whether the second custom view is
reading from bucket 2, or from another half-page inside bucket 1.

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-page-a-red-green.png \
  --bucket2 test-images/cidoo-page-b-blue-yellow.png \
```

Expected if bucket boundaries are correct:

- bucket 1 shows red/green with `A0`/`A1`, likely repeated above and below the
  center seam
- bucket 2 shows blue/yellow with `B0`/`B1`, likely repeated above and below the
  center seam

If both views only show red/green, the device is not using the uploaded bucket 2
frame where we currently place it.

## Two-Frame GIF Test

Run this only after the still bucket test is understood.

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 test-images/cidoo-grid-ab-2frame.gif \
  --bucket2 test-images/cidoo-grid-ab-2frame.gif \
```

Expected summary for this GIF-in-both-buckets test:

```text
final GIF1 frame count: 2
final GIF2 frame count: 2
final total frame count: 4
```
