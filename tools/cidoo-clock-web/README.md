# CIDOO Clock WebHID

This is a browser version of the `cidoo-clock` clock update path. It uses
Chrome/Edge WebHID instead of compiling the C tool to WASM, because the C tool
depends on macOS IOKit/CoreFoundation and WASM cannot directly access USB HID
devices from Chrome.

## Run

Serve the directory from localhost:

```sh
python3 -m http.server 8080 --bind 127.0.0.1 --directory tools/cidoo-clock-web
```

Then open:

```text
http://localhost:8080
```

Use Chrome or Edge. WebHID requires a secure context; `localhost` qualifies.

## Workflow

1. Connect to the default `0x320f:0x5055`, usage page `0xff1c`, usage `0x92`,
   report ID `0x04` device.
2. Read the current record.
3. Confirm `Patch only bytes 35..41`.
4. Update the clock.

The update path re-reads the current 48-byte record before writing, verifies the
stable SHA-256 matches the last read record, validates the existing time fields,
patches only offsets `35..41`, and sends the same `0x01`, `0x06`, `0x02`
sequence used by the C helper.
