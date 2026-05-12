# CIDOO Clock C3 Port

This is a C3 port of the `tools/cidoo-clock.c` clock/config helper. It is kept
in its own project so the existing `.c` files are untouched.

There is no C shim in this port. The transport boundary is C3-only:

- `src/main.c3` owns CLI parsing, protocol packets, hashes, time encoding, and
  update safety checks.
- `src/c_api.c3` contains the generic libc/CommonCrypto extern declarations.
- `src/transport.c3` defines the generic `ClockTransport { kind, impl }` handle.
- `src/macos_iokit_adapter.c3` is the macOS adapter. It contains the
  CoreFoundation/IOKit extern declarations and implements the transport API:
  `transport_open`, `transport_close`, `transport_send_wait`,
  `transport_send_wait_checked`, `transport_get_feature_report`, and
  `transport_list_devices`.

Another transport can keep `main.c3` unchanged by implementing the same
transport API and changing the selected `TransportKind`/open path.

## Build

Install `c3c`, then from this directory run:

```sh
c3c build cidoo-clock-c3
```

The project links against macOS `IOKit.framework` and
`CoreFoundation.framework`, matching the native C helper's HID transport.

If your C3 toolchain does not accept `.framework` entries in
`linked-libraries`, build with explicit framework flags according to that
toolchain's linker option syntax. The C3 project file is otherwise standalone.

## Commands

The port implements the main clock tool surface:

```sh
./build/cidoo-clock-c3 list
./build/cidoo-clock-c3 probe
./build/cidoo-clock-c3 get-feature --allow-hid-query
./build/cidoo-clock-c3 read-raw --allow-hid-query --raw-command 0x05
./build/cidoo-clock-c3 dry-run --template-file template.bin --print-packets
./build/cidoo-clock-c3 read-template --allow-hid-query --slot 0 --out template.bin
./build/cidoo-clock-c3 update-time \
  --allow-hid-query \
  --allow-config-write \
  --expected-stable-sha256 PUT_HASH_FROM_READ_TEMPLATE_HERE \
  --confirm-write PATCH_ONLY_BYTES_35_41
```

`read-template-split` is intentionally not ported; the standard chunked
`read-template` path is the one used by the safe update flow.

## Safety Model

The update path matches the C helper:

- `dry-run` never opens HID.
- `read-template` requires `--allow-hid-query`.
- `update-time` requires `--allow-hid-query`, `--allow-config-write`, the stable
  hash from `read-template`, and `--confirm-write PATCH_ONLY_BYTES_35_41`.
- Before writing, it re-reads the current 48-byte record, checks the stable hash,
  validates the current time fields, patches only offsets `35..41`, verifies no
  other byte changed, then sends `0x01`, `0x06`, `0x02`.

## Verification Note

This repository environment did not have `c3c` installed when the port was
written, so syntax/link verification still needs to be done on a machine with a
C3 compiler. The implementation is deliberately close to the C helper to make
that first compiler pass mechanical.
