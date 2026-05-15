# cdoo WASM WebHID

Minimal browser UI backed by the C3 protocol core compiled to WASM.

Build the WASM core from the repo root:

```sh
c3c --path tools/cidoo-clock-c3 build web-core
cp tools/cidoo-clock-c3/build/cdoo-core.wasm tools/cidoo-clock-web-wasm/cdoo-core.wasm
```

Serve this directory over `localhost` and open it in a WebHID-capable browser. Image upload accepts two `.gif` files and decodes all GIF frames through `ImageDecoder`.
