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

The observed configured chunk size for the ABM066 path is `0x38` bytes:

```text
0x38 = 56 bytes
```

For the padded `65536` byte RGB565 payload, this means:

```text
ceil(65536 / 56) = 1171 packets
```

The app waits about 500 ms after the first successful `0x21` packet whose offset
is zero.

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
| `43` | low byte of a 16-bit UI/config value |
| `44` | high byte of a 16-bit UI/config value |

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

### Safe Time Update Strategy

To minimize brick risk, do not reconstruct the 48-byte record from app UI state
or constants.

The safest inferred strategy is:

1. Obtain the current 48-byte record for the exact slot that will be written.
2. Verify the read returned exactly 48 bytes and that the existing time fields
   at offsets `35..41` decode to plausible BCD/date values.
3. Create the write payload by copying all 48 bytes unchanged.
4. Replace only offsets `35..41` with the new local time/date values.
5. Refuse to write if any other byte differs from the current record.
6. Use command `0x06` at the same slot offset, with the standard outer checksum.

This avoids the official app's broader rewrite of mode/color/layout fields while
still updating the clock bytes the app itself uses.

Important caveat: on this protocol even a read operation requires sending a HID
output report containing command `0x05`. That is a non-mutating query, but it is
still a USB/HID write at the transport layer. Only these read-query output
reports were sent during the live investigation.

## Clock Update Tool

The repo contains a macOS C helper at:

```text
tools/cidoo-clock.c
```

Build it with:

```sh
make -C tools
```

The tool is intentionally gated:

- `dry-run` never opens HID and never sends a report.
- `read-template` refuses to send command `0x05` unless `--allow-hid-query` is
  present.
- `update-time` refuses to write unless all three guards are present:
  `--allow-hid-query`, `--allow-config-write`, and
  `--confirm-write PATCH_ONLY_BYTES_35_41`.
- `update-time` also requires `--expected-stable-sha256`. This hash is computed
  from the 48-byte record with offsets `35..41` zeroed, so it stays stable even
  if the device clock seconds change between `read-template` and `update-time`.
- The clock tool defaults to usage page `0xff1c`, usage `0x92`, and report ID
  `0x04`, matching the Windows app's config protocol.
- `read-template` uses the official `0x482300` 4-byte chunked read helper, so it
  works with the 8-byte input report exposed by macOS.
- The tool also rejects exact 64-byte echoes and all-zero 48-byte payloads, so
  the report ID `0x05` echo path cannot accidentally create a fake template.

Recommended workflow:

```sh
# Sends only non-mutating 0x05 query reports and saves the active slot template.
./tools/cidoo-clock read-template --allow-hid-query --slot 0 --out template.bin

# No device access. Verifies the saved 48-byte template and prints the exact
# command 0x06 packet that would be sent.
./tools/cidoo-clock dry-run --template-file template.bin --print-packets

# Writes only after re-reading the current 48-byte record, checking the stable
# hash from read-template, patching only bytes 35..41, and verifying that no
# other payload byte changed.
./tools/cidoo-clock update-time \
  --allow-hid-query \
  --allow-config-write \
  --expected-stable-sha256 PUT_HASH_FROM_READ_TEMPLATE_HERE \
  --confirm-write PATCH_ONLY_BYTES_35_41
```

Current safety conclusion: slot `0` can be read as a real 48-byte template on
macOS using the chunked read helper. Slots `1` and `2` should not be used for a
time update based on the current evidence. The all-zero records produced through
report ID `0x05` are rejected and must not be used as write templates.

For numbered reports on macOS, the tool passes the full documented 64-byte
packet to `IOHIDDeviceSetReport` while also specifying report ID `0x04`. The
diagnostic option `--strip-report-id-for-iohid` is available for experiments,
but it is not the default because many macOS HID paths expect the report ID byte
to remain in the buffer.

## Safety Notes

Do not send HID output or feature reports to the keyboard unless deliberately
testing with user approval.

For a macOS implementation, start with a dry-run mode that:

1. Parses an image or screen JSON into `240 x 135` RGB pixels.
2. Converts to padded RGB565 payload.
3. Builds command `0x01`, `0x21`, and `0x02` packets.
4. Prints packet counts, checksums, and first/last packets without opening HID.

Only after dry-run verification should a real HID write mode be enabled.

## Image Upload Tool

The repo also contains a macOS C helper for screen image uploads:

```text
tools/cidoo-image.c
```

Build it with:

```sh
make -C tools
```

The tool loads a host image through macOS ImageIO, fits it to a logical display
canvas, rotates that canvas into the protocol framebuffer when needed, converts
RGB888 to big-endian RGB565, pads the payload to `65536` bytes, and builds the
confirmed command sequence:

```text
0x01 begin
0x21 image chunks, 56 bytes per packet
0x02 commit
```

It is intentionally gated:

- `dry-run` never opens HID and never sends a report.
- `upload` refuses to send any report unless both guards are present:
  `--allow-image-write` and `--confirm-upload UPLOAD_SCREEN_IMAGE`.
- The default HID target is usage page `0xff1c`, usage `0x92`, report ID `0x04`.
- `--orientation landscape` uses a `240 x 135` physical preview and no rotation.
- `--orientation portrait` uses a `135 x 240` physical preview and rotates it
  into the required `240 x 135` framebuffer. The default portrait rotation is
  `90`; use `--rotate 270` if the first test appears upside down or sideways on
  the device.

Recommended portrait dry-run:

```sh
./tools/cidoo-image dry-run \
  --image path/to/image.png \
  --orientation portrait \
  --fit cover \
  --preview-out /private/tmp/cidoo-physical-preview.png \
  --framebuffer-preview-out /private/tmp/cidoo-framebuffer-preview.png \
  --print-packets
```

Expected ABM066 image payload properties:

```text
physical preview: 135x240
rotation into framebuffer: 90
framebuffer: 240x135
rgb565 bytes: 64800
padded bytes: 65536
chunk bytes: 56
image packet count: 1171
```

The dry-run should end with:

```text
No HID device was opened. No report was sent.
```

The physical preview PNG is what should appear upright on the device. The
framebuffer preview PNG is the rotated `240 x 135` raster that is actually
encoded into RGB565.

Important current limitation: the legacy `upload --image` command only models
the raw one-frame stream and is intentionally blocked for real writes. Use the
two-bucket commands below for hardware uploads, because they update the
48-byte config metadata and image payload together.

Upload command, only after inspecting dry-run output:

```sh
./tools/cidoo-image upload \
  --image path/to/image.png \
  --orientation portrait \
  --fit cover \
  --preview-out /private/tmp/cidoo-physical-preview.png \
  --framebuffer-preview-out /private/tmp/cidoo-framebuffer-preview.png \
  --allow-image-write \
  --confirm-upload UPLOAD_SCREEN_IMAGE \
  --print-packets
```

## Multi-Frame / Two Custom Screen Images

The Windows UI has two custom screen buckets:

- `Custom1`, shown in the side panel as `GIF1`
- `Custom2`, shown in the side panel as `GIF2`

The UI resources define both buckets even in the default data:

- `app/DefaultData/ScreenFrame1.json`
- `app/DefaultData/ScreenFrame2.json`

Each frame-list JSON contains a `Frame` array whose entries name per-frame pixel
JSON files. A still image is represented as a one-frame animation.

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

So two still images, one in `GIF1` and one in `GIF2`, would send:

```text
2 * 65536 = 131072 bytes
ceil(131072 / 56) = 2341 command 0x21 packets
```

Before handing the combined payload to the async image worker, the Windows app
updates the 48-byte config record fields that describe the custom-image storage:

| Config offset | Windows source | Meaning inferred from use |
| ---: | --- | --- |
| `33` | selected custom bucket plus `1` in the all-frame path | active/custom image selector for all-frame upload |
| `34` | `ScreenFrame1.json` frame count | `GIF1` frame count |
| `46` | `ScreenFrame2.json` frame count | `GIF2` frame count |

These are offsets inside the 48-byte payload copied from object offset `0x235f`.
They correspond to object fields:

```text
object + 0x2380 -> config[33]
object + 0x2381 -> config[34]
object + 0x238d -> config[46]
```

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

### Safe Multi-Image Strategy

To safely store two still images for later toggling as `Custom1` / `Custom2`,
the uploader should not synthesize config bytes from constants. It should:

1. Read the current active 48-byte config template with the same chunked
   command `0x05` path used by `cidoo-clock`.
2. Build one padded frame for each still image.
3. Concatenate frames in official order: all `GIF1` frames, then all `GIF2`
   frames.
4. Copy the 48-byte template unchanged.
5. Patch only the image metadata bytes needed for the chosen layout:
   `33`, `34`, and `46`. If matching Windows exactly, also patch clock bytes
   `35..41` to the current local time because the official config writer does.
6. Dry-run must print the before/after config diff and refuse unless the only
   config changes are the expected offsets.
7. Only then send the command sequence above.

This path is implemented in `tools/cidoo-image.c` as `dry-run-buckets` and
`upload-buckets`. The legacy single-image `upload` command still refuses to
write.

The dry-run command can inspect either one bucket or both buckets, but real
uploads must write both buckets together.

- `--bucket1 PATH --bucket2 PATH`: writes the full Windows-style combined stream
  from frame offset `0`.
- Partial upload attempts are disabled. A failed `GIF2`-only test strongly
  indicates that command `0x23` operates on, erases, or reinitializes the
  combined custom-image store rather than safely preserving the untouched
  bucket. This matches the Windows app behavior: it uploads all frames from
  `GIF1` followed by all frames from `GIF2`.

Dry-run with a freshly read 48-byte template:

```sh
./tools/cidoo-clock read-template \
  --allow-hid-query \
  --out cidoo-template.bin

./tools/cidoo-image dry-run-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --template-file cidoo-template.bin \
  --orientation portrait \
  --print-packets
```

For the Windows-equivalent behavior, omit `--preserve-clock`; the tool refreshes
clock bytes `35..41` the same way the Windows config writer does. With
`--preserve-clock`, the dry-run must show only offsets `33`, `34`, and `46`
changed.

Real upload requires all gates:

```sh
./tools/cidoo-image upload-buckets \
  --bucket1 path/to/gif1.gif \
  --bucket2 path/to/gif2.gif \
  --orientation portrait \
  --allow-hid-query \
  --allow-config-write \
  --allow-image-write \
  --expected-stable-sha256 TEMPLATE_STABLE_SHA256_FROM_DRY_RUN_OR_READ_TEMPLATE \
  --confirm-upload UPLOAD_SCREEN_IMAGE \
  --print-packets
```

The uploader refuses all-zero templates, re-reads the current config record
before writing, verifies the current stable hash, verifies that only expected
config offsets changed, then sends:

```text
0x01, 0x06, 0x02, 0x23, repeated 0x21, 0x02
```

The tool currently caps input at 64 frames per bucket by default, accepts
`--max-frames-per-bucket` up to 255, and refuses more than 255 total frames so
the 24-bit image-packet offset stays inside the understood range.
