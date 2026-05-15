# CIDOO Clock C3 Port

This is a C3 port of the `tools/cidoo-clock.c` clock/config helper. It is kept
in its own project so the existing `.c` files are untouched.

There is no C shim in this port. The transport boundary is C3-only:

- `src/main.c3` owns protocol packets, hashes, time encoding, and update safety
  checks. It uses the C3 standard library for console output, SHA-256, local
  time, monotonic timeouts, prompts, memory copies, and the small
  `ClockTransport { kind, impl }` handle used by the macOS adapter.
- `lib/cli.c3` owns the small public option surface.
- `src/macos_iokit_adapter.c3` is the macOS adapter. It contains the
  CoreFoundation/IOKit extern declarations and implements the transport API:
  `transport_open`, `transport_close`, `transport_send_wait`,
  `transport_send_wait_checked`, `transport_get_feature_report`, and
  `transport_list_devices`.

Another transport can keep `main.c3` unchanged by implementing the same
transport API and using a different `TransportKind` value.

The project has no vendored C3 dependencies.

## Build

Install `c3c`, then from this directory run:

```sh
c3c build cdoo
```

The project links against macOS `IOKit.framework`, `CoreFoundation.framework`,
`CoreGraphics.framework`, and `ImageIO.framework`, matching the native C
helper's HID transport and image decode path.

If your C3 toolchain does not accept `.framework` entries in
`linked-libraries`, build with explicit framework flags according to that
toolchain's linker option syntax. The C3 project file is otherwise standalone.

## Commands

The port implements the main clock tool surface:

```sh
./build/cdoo list
./build/cdoo probe
./build/cdoo get-feature
./build/cdoo read-raw
./build/cdoo read-template
./build/cdoo update-time
./build/cdoo update-time --dry-run
./build/cdoo update-time --time 2026-05-15T12:34:56
./build/cdoo upload-image gif1.png gif2.png
./build/cdoo upload-image --dry-run gif1.png gif2.png
./build/cdoo update-time --dry-run --debug
```

The supported options are `--slot`, `--time`, `--dry-run`, `--debug`,
`--timeout-ms`, and `--help`. `upload-image` takes exactly two image paths for
the device's GIF1 and GIF2 custom buckets. HID VID/PID, usage page, usage, and
report ID are internal protocol constants for the CIDOO clock path.

## Safety Model

The update path keeps the C helper's protocol boundaries with a smaller CLI:

- `read-template` and `read-raw` use the confirmed chunked command `0x05`
  read path. Add `--debug` to print the HID packets and responses.
- `update-time` reads the current 48-byte record from the device just before
  patching, validates the current time fields, patches only offsets `35..41`,
  verifies no other byte changed, then asks for interactive confirmation before
  sending `0x01`, `0x06`, `0x02`.
- `update-time --dry-run` still reads the current device record and computes
  the patched record, but it does not send the write sequence.
- `upload-image` decodes both image paths, reads the current 48-byte record,
  patches only the active selector and GIF1/GIF2 frame-count metadata, writes
  the config, verifies the readback stable hash, then streams the combined
  image store with `0x23`, `0x21`, and `0x02`.
- `--debug` prints packet dumps and changed byte offsets.

## Verification

The port builds with C3 0.8.0. The clock update path uses the same packet
builders, checksum layout, BCD time encoding, and byte-preservation checks as
the reference C helper.
