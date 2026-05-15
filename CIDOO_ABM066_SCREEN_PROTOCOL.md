# CIDOO ABM066 Screen Upload Protocol

Reverse engineering notes for the CIDOO ABM066 Windows "Image Custom Tool".

These notes are from static analysis of `app/Image Custom Tool.exe`, read-only
USB/HID enumeration on macOS, and read-only HID query commands. No config writes,
image uploads, begin/commit commands, or flash operations were sent to the
keyboard during this analysis.

## Device

Observed connected keyboard:

- Product: `CIDOO ABM066`
- Manufacturer: `Telink`
- USB VID: `0x320f` decimal `12815`
- USB PID: `0x5055` decimal `20565`
- bcdDevice: `0x0105`

The bundled `DefaultData/Keyboard.json` contains generic values:

- VID `3141` (`0x0c45`)
- PID `4103` (`0x1007`)
- Chip model `VS11K09A`

Those bundled IDs do not match the actual ABM066 seen on macOS.

## HID Interfaces

macOS exposes multiple HID collections for the keyboard.

The Windows app's screen/config protocol uses this vendor-defined collection:

- Usage page: `0xff1c`
- Usage: `0x92`
- Report ID: `0x04`
- Output report size: 64 bytes including report ID
- Input report size: descriptor says 8 bytes including report ID

Descriptor seen for that collection:

```text
061cff0992a101850419002aff00150026ff007508953f910019002aff00750895078100c0
```

This is enough for macOS to send command `0x05` and receive the 8-byte response
used by the Windows app's short read helper at `0x482300`. It is not enough to
receive the single-report 48-byte response expected by the separate helper at
`0x482520`.

macOS also exposes a keyboard collection:

- Usage page: `0x0001`
- Usage: `0x06`
- Report ID: `0x05`
- Input report size: 64 bytes including report ID
- Output report size: 64 bytes including report ID

Descriptor seen for that collection includes report ID `0x05` with 63 bytes of
input, output, and feature data:

```text
06efff0900a1018505150026ff00953f750809018102090291020903b102c0
```

Live tests show this `0x0001/0x06`, report ID `0x05` collection is not the
screen/config template path. A command `0x05` packet sent there is echoed back as
a 64-byte report, producing an all-zero 48-byte "payload" if bytes `8..55` are
naively copied. That all-zero result must not be treated as a valid template.

There is also a `0xff60/0x61` collection with 32-byte input/output reports. It
is not large enough for the 48-byte clock/config read response.

## Screen Format

The app's default screen JSON files are:

- `app/DefaultData/e_ScreenCustom1_0.json`
- `app/DefaultData/e_ScreenCustom2_0.json`

Each contains:

- `BkGroundColor`
- `PenColor`
- `Pixel`

`Pixel` contains `32400` entries, which is:

```text
240 * 135 = 32400
```

Each JSON pixel is an object:

```json
{"Red": 0, "Green": 0, "Blue": 0, "Flag": 0}
```

The Windows app stores the internal screen framebuffer as an 8-byte-per-pixel
structure at object offset `0x3df28`, with width at `0x28b4` and height at
`0x28b8`. The row stride is `0x780`, which is:

```text
240 * 8 = 0x780
```

The Windows UI drawing area is `480 x 270`, exactly `2x` the `240 x 135`
logical framebuffer. Live ABM066 testing showed that an unrotated `240 x 135`
payload displays sideways on the physical screen, so host-side portrait
pre-rotation is required for this device.

The Windows imported-image rotate path is gated by object field `0x28bc`. When
enabled, function `0x4b02b0` calls OpenCV `transpose`, then `flip(..., 0)`, then
resizes to object fields `0x28b4 x 0x28b8`. That transform is equivalent to a
`270` degree rotation in the local uploader's convention.

## Upload Pixel Encoding

The confirmed image upload path converts RGB888 to RGB565:

```c
rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
```

The app writes the RGB565 word to the upload buffer high byte first:

```text
payload[i * 2 + 0] = rgb565 >> 8
payload[i * 2 + 1] = rgb565 & 0xff
```

For a `240 x 135` image:

```text
240 * 135 * 2 = 64800 bytes
```

The app pads this to a 32768-byte boundary. For the ABM066 default dimensions,
the upload buffer becomes:

```text
65536 bytes
```

Both confirmed Windows packers use ordinary row-major pixel order:

```text
pixel_index = y * width + x
payload_offset = pixel_index * 2
```

No odd/even row interleave, column-major order, or serpentine scan mapping has
been found in the Windows upload path.

Live ABM066 tests show an additional device-side layout for the visible custom
views. The firmware appears to display `32768` byte device frames. Community
notes identify the actual screen resolution as `96x160`; live artifacts indicate
the device reads a landscape `160x96` memory frame and rotates it onto the
portrait LCD. The current working layout candidate is:

```text
physical source       = 96 x 160 pixels
device memory frame   = 160 x 96 pixels
row stride            = 320 bytes
pixel bytes           = 160 * 96 * 2 = 30720 bytes
device frame stride   = 32768 bytes
trailing padding      = 2048 bytes
```

Earlier `135x120` tests produced staircase artifacts. Changing that candidate
from tight `270` byte rows to `272` byte rows made the red/green boundary
interleave worse. A `128x128` candidate matched the `0x8000` byte frame size but
does not match the reported `3:5` physical screen ratio. Writing a native
`96x160` source directly in portrait row-major order put source top/bottom
halves on opposite sides of the display and spread corner markers through the
height, matching a landscape-memory interpretation.

This is distinct from the Windows host-side all-frame buffer, which still uses
`240x135` source frames padded to `65536` bytes before handing the buffer to the
upload worker.

## HID Packet Format

All confirmed protocol packets in this family are 64 bytes.

Common packet layout:

```text
byte 0      report id = 0x04
byte 1      checksum low byte
byte 2      checksum high byte
byte 3      command
byte 4..63  command-specific payload
```

Checksum:

```text
checksum = sum(packet[3..63]) & 0xffff
packet[1] = checksum & 0xff
packet[2] = checksum >> 8
```

The Windows app sends packets through an async `WriteFile` wrapper and then reads
a response through an async `ReadFile` wrapper.

Response handling:

- Response byte `7 == 0xff` maps to error `0xffffff9a`
- Response byte `7 == 0xfe` maps to error `0xffffff99`
- Otherwise the operation is treated as successful

The generic transaction wrapper also expects the response command byte to match
the request command byte.

## Screen Upload Sequence

The confirmed screen upload path is:

1. Begin session: command `0x01`
2. Upload image chunks: command `0x21`
3. Commit/end session: command `0x02`

The relevant Windows code path:

- Converts screen framebuffer to RGB565 near `0x4adb20`
- Sends begin command via `0x47fd80`
- Sends image data via `0x481850`
- Sends commit command via `0x47ff90`

### Command 0x01: Begin

Packet fields:

```text
byte 0 = 0x04
byte 3 = 0x01
byte 4 = 0x00
byte 5 = 0x00
byte 6 = 0x00
byte 7 = 0x00
```

The rest of the packet is zero, then checksum is computed over bytes `3..63`.

### Command 0x02: Commit

Packet fields:

```text
byte 0 = 0x04
byte 3 = 0x02
byte 4 = 0x00
byte 5 = 0x00
byte 6 = 0x00
byte 7 = 0x00
```

The Windows app sleeps for about 10 ms before sending this packet.

### Command 0x21: Image Data

Packet fields:

```text
byte 0 = 0x04
byte 3 = 0x21
byte 4 = chunk length
byte 5 = payload offset low byte
byte 6 = payload offset middle byte
byte 7 = payload offset high byte
byte 8..63 = payload chunk
```

The Windows sender stores its command `0x21` chunk length in the protocol object.
Static analysis found two configured modes:

```text
0x18 = 24 bytes
0x38 = 56 bytes
```

The protocol object constructor initializes the value to `0x18`. A setter at
`0x482c30` can switch between `0x18` and `0x38`. Live ABM066 tests with the
local `0x38` sender produced recognizable but regularly cut/interlaced images,
which is consistent with a receiver expecting `0x18`-byte addressed chunks while
the host advances offsets by `0x38`. The local uploader therefore defaults to
the constructor value, `0x18`.

For the padded `65536` byte RGB565 payload, the default `0x18` mode means:

```text
ceil(65536 / 24) = 2731 packets
```

The Windows app waits about 500 ms after the first successful `0x21` packet
whose offset is zero. The local uploader does not currently include this
host-side sleep; confirmed ABM066 image writes work without it.

In the all-frame sender at `0x481850`, the payload offset is a local loop
counter initialized to zero and incremented by the sent chunk length. No caller
argument supplies a nonzero start offset. For the Windows two-bucket path, image
data therefore always starts at byte offset `0` of the combined custom-image
store.

Example first image packet for an all-black image:

```text
04 59 00 21 38 00 00 00 00 00 00 00 ...
```

Where:

- `04` is report ID
- `59 00` is checksum `0x0059`
- `21` is command
- `38` is chunk length
- `00 00 00` is offset zero
- payload bytes are all zero for black RGB565

## Other Commands Seen

These commands exist in the same protocol family and are used for device
configuration data outside the confirmed screen image upload path:

| Command | Function | Notes |
| --- | --- | --- |
| `0x03` | `0x4803c0`, `0x480680` | Reads device data/config |
| `0x05` | `0x482300`, `0x482520` | Reads config records |
| `0x06` | `0x482080` | Writes config records |
| `0x07` | `0x4810f0` | Reads data into caller buffer |
| `0x09` | `0x480960` | Chunked write, likely non-screen data |
| `0x0b` | `0x481b20` | Unknown |
| `0x12` | `0x481dc0` | Lighting/color related, write-only path |
| `0x15` | `0x480c20` | Chunked write, likely macro/layout data, not screen image |
| `0x17` | `0x480eb0` | Chunked write with 56-byte chunks |
| `0x1a` | `0x4827b0` | Unknown |
| `0x1b` | `0x481390` | Reads data into caller buffer |
| `0x1c` | `0x4801b0` | Small command |
| `0x23` | `0x481640` | Related setup command using a variant transaction wrapper |

## Command 0x05: Read Config Record

The Windows app reads a 48-byte screen/config record from the keyboard before it
constructs the data sent by command `0x06`. Static analysis shows the record is
cached at object offset `0x235f` and is copied back into the command `0x06`
payload as the base template.

The Windows app contains two command `0x05` helpers.

Helper `0x482520` requests the caller's full length in one transaction and
copies returned payload from response offset `8`. This needs a response long
enough to hold the command header plus payload.

Helper `0x482300` reads the caller's buffer in chunks of at most 4 bytes and
copies returned payload from response offset `4`. This fits the 8-byte input
report exposed by macOS for report ID `0x04`.

Request packet:

```text
byte 0 = 0x04
byte 3 = 0x05
byte 4 = requested length
byte 5 = read offset low byte
byte 6 = read offset high byte
byte 7 = 0x00
```

The checksum is the same `sum(packet[3..63]) & 0xffff` checksum used by the
image upload packets.

Response handling in `0x482520`:

```text
byte 7      status/error byte
byte 8..   returned payload
```

Status values:

- `0xff` maps to error `0xffffff9a`
- `0xfe` maps to error `0xffffff99`
- any other value is treated as success

Response handling in `0x482300`:

```text
byte 4..7   returned payload chunk, up to 4 bytes
```

This helper does not use byte `7` as a status byte; it can be legitimate payload.

The helper supports arbitrary 16-bit offsets. One official app startup/read path
first reads 1 byte from offset `0`, treats that byte as the active slot index,
then reads 48 bytes from:

```text
offset = slot_index * 0x31
```

Another path reads 48 bytes from offset `0` directly.

### macOS Read Results

Read-only command `0x05` queries sent to the confirmed `0xff1c/0x92`, report ID
`0x04` collection return the 8-byte input report that macOS exposes. A full
48-byte request therefore only exposes the first four payload bytes:

```text
offset 0x00, length 0x30 -> 04 35 00 05 00 0c 05 01
offset 0x31, length 0x30 -> 04 66 00 05 00 00 00 00
offset 0x62, length 0x30 -> 04 97 00 05 00 00 00 00
```

Using the official 4-byte helper shape, macOS can reconstruct the template from
response bytes `4..7`.

Slot `0`, offset `0x00`, returned this 48-byte record:

```text
00 0c 05 01 00 00 c8 00 ff 23 00 00 00 00 00 00
00 00 00 00 ff 00 00 00 00 00 00 ff 03 00 00 00
0f 00 14 00 49 18 04 01 08 24 00 64 00 00 00 0f
```

Hashes:

```text
sha256        46a9f0477b74c1ef19ca2cfdcc4eacd984133374fa9c28f30f344d909c97f972
stable sha256 52f6359f6a60e22bcd8ddca80b43721d8d80324855f452bfd293d06c2f878be4
```

The time bytes decode as `2024-08-01 18:49:00`, weekday `4`, which explains the
wrong clock view. The first byte read at offset `0` is `0x00`, so the active slot
appears to be slot `0`.

Slot `1`, offset `0x31`, returned a sparse record with byte `15 = 0xff` and
invalid zero date fields. Slot `2`, offset `0x62`, returned all zeroes and is
rejected by the tool.

The `0x0001/0x06`, report ID `0x05` collection returns a 64-byte echo of the
request packet for command `0x05`. Copying bytes `8..55` from that echo produces
48 zero bytes, but those bytes are not evidence that any keyboard slot is all
zero. They are just the unused payload area of the echoed request.

## Command 0x06: Write Config Record

The Windows app writes the same family of config records through `0x482080`.

Request packet:

```text
byte 0 = 0x04
byte 3 = 0x06
byte 4 = chunk length
byte 5 = write offset low byte
byte 6 = write offset high byte
byte 7 = 0x00
byte 8..63 = payload chunk
```

For the 48-byte record, the official app sends a single `0x30` byte payload when
the HID chunk size is the usual wired `0x38` bytes.

The app wraps the write with:

1. Optional begin command `0x01`
2. Command `0x06` payload write
3. Commit command `0x02`

The command `0x06` write offset is:

```text
offset = slot_index * 0x31
```

The payload length is 48 bytes (`0x30`) even though the slot stride is 49 bytes
(`0x31`). The extra byte in the stride has not been identified yet.

## 48-Byte Screen/Clock Config Record

The official app builds the command `0x06` 48-byte payload in function
`0x49f5c0`.

Important safety finding: the app does not create this record from constants.
It first copies the current 48-byte device record read by command `0x05`, then
overwrites selected fields.

Template source:

```text
template = device config bytes read with command 0x05
payload[0..47] = template[0..47]
```

The official app then overwrites these byte offsets before sending:

| Offset | Source / meaning |
| ---: | --- |
| `0` | caller argument byte |
| `1` | mode/value from UI config object, mapped through table at object offset `0x1a60`; values `0x1d` and `0x1e` are forced to `0xfe` |
| `2` | UI config byte |
| `3` | `4 - UI config byte` |
| `4` | UI config byte |
| `5` | UI config byte |
| `6` | UI config byte |
| `7` | UI config byte |
| `8` | UI config byte |
| `16` | forced to `0x00` |
| `18` | UI config byte |
| `19` | UI config byte |
| `20` | UI config byte |
| `21` | UI config byte |
| `22` | UI config byte |
| `25` | UI config byte |
| `35` | seconds, packed BCD |
| `36` | minutes, packed BCD |
| `37` | hours, packed BCD, 24-hour local time |
| `38` | weekday, Sunday `0` through Saturday `6` |
| `39` | day of month, packed BCD |
| `40` | month, packed BCD, January `0x01` through December `0x12` |
| `41` | two-digit year, packed BCD |
| `43` | `FrameIntervalTime` low byte, little-endian milliseconds |
| `44` | `FrameIntervalTime` high byte, little-endian milliseconds |

All other offsets are preserved from the 48-byte template:

```text
9..15, 17, 23..24, 26..34, 42, 45..47
```

### Time Encoding

Time/date values are copied from the host's local time at send time.

Packed BCD:

```text
bcd(n) = ((n / 10) << 4) | (n % 10)
```

Example for Monday, May 11, 2026 at 15:04:09 local time:

```text
payload[35] = 0x09  seconds
payload[36] = 0x04  minutes
payload[37] = 0x15  hours
payload[38] = 0x01  Monday
payload[39] = 0x11  day
payload[40] = 0x05  month
payload[41] = 0x26  year
```

### Frame Interval Encoding

The bundled `app/DefaultData/Keyboard.json` contains:

```json
"FrameIntervalTime":100
```

The known-good 48-byte device template stores bytes `43..44` as:

```text
64 00
```

That is little-endian decimal `100`. Static analysis of `0x49f5c0` confirms
those two payload bytes are written from object field `0x2848`.

The protocol stream carries frame count metadata, not per-frame timing metadata.
The current evidence points to one global frame interval in the config record,
not individual GIF frame delays.

Community notes recommend exporting GIFs at `96x160`, `16` fps and setting the
keyboard software interval to `60` ms. If encoded in the config, `60` ms would
be bytes `3c 00` at offsets `43..44`.

### Config Mutation Boundaries

The official config writer reconstructs many UI-derived bytes before sending
command `0x06`. The clock-only update boundary inferred from the data format is
to preserve the current 48-byte record and replace only offsets `35..41`.

The custom image metadata boundary inferred from the all-frame path is to
preserve the current 48-byte record and replace offsets `34` and `46`. Windows
also writes offset `33` from an in-memory selector value and refreshes clock
offsets `35..41` when it writes this record. Live ABM066 testing showed a
known-good template with offset `33 = 0`, so a local uploader should preserve
offset `33` unless the selector semantics are deliberately being tested.

On this protocol even a read operation requires sending a HID output report
containing command `0x05`. That is a non-mutating query at the protocol level,
but it is still a USB/HID write at the transport layer.

## Multi-Frame / Two Custom Screen Images

The Windows UI has two custom screen buckets:

- `Custom1`, shown in the side panel as `GIF1`
- `Custom2`, shown in the side panel as `GIF2`

The UI resources define both buckets even in the default data:

- `app/DefaultData/ScreenFrame1.json`
- `app/DefaultData/ScreenFrame2.json`

Each frame-list JSON contains a `Frame` array whose entries name per-frame pixel
JSON files. A still image is represented as a one-frame animation.

### Windows Image/GIF Import Path

The Windows app imports still images and animated/video sources through OpenCV
3.2 (`opencv_world320.dll`).

Still-image import:

- Function path beginning near `0x4afd4e`
- Calls `cv::imread(path, 1)`, i.e. color image import
- Optionally calls `cv::transpose` and `cv::flip` when object field `0x28bc` is
  set
- Calls `cv::resize` to the screen dimensions when the imported image size does
  not already match object fields `0x28b4` and `0x28b8`
- The confirmed path is a direct stretch-to-fit resize; no aspect-ratio
  preserving contain/cover behavior has been found in this path
- Adds the resulting frame to the screen-frame list

Animated/video import, including GIFs:

- Function path beginning near `0x4b0550`
- Constructs `cv::VideoCapture`
- Calls `VideoCapture::open(path)`
- Checks `VideoCapture::isOpened()`
- Repeatedly calls `VideoCapture::read(Mat)` until it fails
- Caps the stored frame count to object field `0x28ac`
- Runs every decoded `Mat` through function `0x4b02b0`, which uses the same
  optional transpose/flip and resize pipeline as still images

This means the official GIF path is a video-frame decode path before frames are
converted into the app's screen-frame/pixel representation. GIF files with
subrectangle frames or disposal rules depend on OpenCV's video decoder behavior
before reaching the CIDOO protocol layer.

### Windows "Upload All Frames" Path

The official all-frame upload path is function `0x4ade00`.

It reads both frame lists, converts every frame to the same padded RGB565 format
used by the single-image path, and concatenates all frames into one image stream:

```text
combined_payload =
  GIF1 frame 0, GIF1 frame 1, ...
  GIF2 frame 0, GIF2 frame 1, ...
```

For the ABM066 dimensions, each frame occupies:

```text
240 * 135 * 2 = 64800 bytes
padded frame size = 65536 bytes
```

Static analysis confirms this padding is per frame, not only at the end of the
combined stream. Function `0x4ade00` computes a padded frame stride by rounding
the raw RGB565 frame size up to the next `32768`-byte boundary, allocates
`frame_stride * total_frames`, and copies each converted frame to:

```text
combined_payload + frame_index * frame_stride
```

So two still images, one in `GIF1` and one in `GIF2`, would send:

```text
2 * 65536 = 131072 bytes
ceil(131072 / 24) = 5462 command 0x21 packets
```

Live ABM066 note: the observed custom-view frame boundary is `32768` bytes, not
`65536` bytes. If bucket 2 is placed at offset `0x10000`, both custom views show
content from the first host frame halves. If bucket 2 is placed at offset
`0x8000`, the second custom view shows bucket 2 content. The current local
uploader therefore packs live ABM066 bucket frames as `32768` byte device frames:

```text
2 * 32768 = 65536 bytes
ceil(65536 / 24) = 2731 command 0x21 packets
```

The mismatch between the Windows host-side `65536` byte frame stride and the
ABM066 live `32768` byte device stride is not fully reconciled yet.

Before handing the combined payload to the async image worker, the Windows app
updates the 48-byte config record fields that describe the custom-image storage:

| Config offset | Windows source | Meaning inferred from use |
| ---: | --- | --- |
| `33` | object field `0x28a4 + 1` in the all-frame path | active/custom image selector byte; `0` is valid on known-good ABM066 template |
| `34` | `ScreenFrame1.json` frame count | `GIF1` frame count |
| `46` | `ScreenFrame2.json` frame count | `GIF2` frame count |

These are offsets inside the 48-byte payload copied from object offset `0x235f`.
They correspond to object fields:

```text
object + 0x2380 -> config[33]
object + 0x2381 -> config[34]
object + 0x238d -> config[46]
```

The exact offset `33` semantics are still not fully resolved. Static analysis
shows the all-frame path writes `object[0x28a4] + 1`, while the single-image path
writes `object[0x28a4]` directly. Because `0xff + 1` wraps to `0x00`, the
known-good `0` value is consistent with an in-memory selector of `-1` or
"default/no explicit custom selection".

The same config writer is function `0x49f5c0`, already identified for the clock
work. It sends command `0x06` with a 48-byte payload and offset:

```text
slot_index * 0x31
```

Because this function is the general config writer, it also refreshes clock
bytes `35..41` from host local time when Windows calls it.

### All-Frame Command Sequence

The all-frame path is not just the current `0x01 -> 0x21 -> 0x02` sequence.
The Windows code path is:

1. Begin config session with command `0x01`
2. Write the 48-byte config record with command `0x06`
3. Commit config session with command `0x02`
4. Start/prepare image transfer with command `0x23`
5. Send the combined frame payload with command `0x21`
6. Commit image transfer with command `0x02`

Command `0x23` has the same zero-payload packet shape as the simple begin
commands, but command byte `3` is `0x23`.

Before command `0x23`, the app calls function `0x482ca0(total_frames)`, which
stores the combined frame count at protocol object offset `0x658`. Command
`0x23` then uses the variant transaction wrapper at `0x47dbd0`.

That wrapper computes its response timeout from the stored frame count:

```text
timeout_ms = total_frames * 0x12c + 0x1f4
timeout_ms = total_frames * 300 + 500
```

Examples:

```text
16 total frames -> 5300 ms
28 total frames -> 8900 ms
```

The async worker at `0x474f4a` calls `0x481640` for command `0x23`, then calls
`0x481850` to stream command `0x21` packets. It does not branch on the return
value from command `0x23`.

The command `0x21` sender does branch on per-packet transaction failures. If a
`0x21` packet transaction fails, the sender returns that error to the async
worker and the worker does not send the final command `0x02` image commit.

The config metadata write and image-data write are separate protocol actions.
The 48-byte config record stores selector/count/timing metadata, not image
bytes. The custom-image frame bytes live in a separate image store written by
command `0x21`.

That means an interrupted all-frame operation can leave the keyboard with config
metadata that points at frame counts or active buckets whose image bytes were not
fully programmed. Restoring a 48-byte config record can restore metadata, but it
does not reconstruct custom-image bytes that were erased or overwritten in the
separate image store.

### Two-Bucket Semantics

The official Windows all-frame operation treats the custom-image flash region as
one combined store:

```text
GIF1 frames first, then GIF2 frames
command 0x21 byte offset starts at 0
```

There is no confirmed protocol field for "start writing at GIF2". The sender's
offset counter starts at zero, and command `0x23` appears to prepare or
reinitialize the combined store before data packets are sent.

Current unresolved details:

- Whether command `0x23` erases the full combined custom-image store or only
  starts a flash/programming mode.
- Whether command `0x23` needs to return a response on every firmware version,
  since the Windows async worker sends command `0x21` data even when command
  `0x23` reports an error to its caller.
- The exact OpenCV backend behavior for GIF subrectangle/disposal compositing on
  the bundled Windows runtime.
- The exact display meaning of config offset `33`. The current local tool
  preserves it by default because forcing it away from the known-good value `0`
  correlated with split/striped custom-image output during live tests.
- Whether any ABM066 firmware/config variant sets the imported-image rotate flag
  at object field `0x28bc`; live hardware behavior indicates the ABM066 needs
  the equivalent host-side portrait pre-rotation.
- Live mapping tests with a nine-band physical X ruler showed one custom view
  displaying approximately bands `0..4` and the other displaying bands `4..8`
  from the same uploaded image. A later A/B bucket test showed no visible `B`
  labels in the second custom view when bucket 2 was placed at byte `0x10000`.
  A red/green versus blue/yellow boundary test then showed view 1 reading the
  first `0x8000` bytes and view 2 reading the second `0x8000` bytes of the first
  uploaded host frame. The local uploader now places bucket frames on `0x8000`
  byte boundaries by default. Community notes identify the physical screen as
  `96x160`; live artifacts indicate the device reads a `160x96` RGB565 memory
  frame and rotates it onto the portrait LCD. That gives `30720` RGB565 bytes
  plus `2048` padding bytes inside each `0x8000` device frame. The earlier
  portrait-row-major, `135x120`, and `128x128` candidates produced
  staircase/interleave artifacts or had the wrong aspect ratio.
