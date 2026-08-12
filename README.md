# Tab5 Longbridge Stock Terminal

Read-only stock quote terminal for M5Stack Tab5 with keyboard kit. The firmware is an ESP-IDF application that stores credentials on-device, connects directly to Longbridge OpenAPI, and renders a watchlist-first quote dashboard for US, Hong Kong, Shanghai, and Shenzhen symbols.

## Status

This repo now contains the ESP-IDF project scaffold, domain model, settings persistence, OAuth PKCE flow, local OAuth callback server, Longbridge socket-token client, Longbridge quote WebSocket client, Longbridge v1 frame/quote protobuf payload codec, LVGL terminal UI, and host unit tests for the portable core logic.

The runtime path opens the quote WebSocket with `/v1/socket/token`, sends AUTH, pulls full snapshots with the socket `QuotePull` command, subscribes with `SubType.Quote`, and merges sparse `PushQuoteData` updates into the local quote store. Real Tab5 boot validation has passed through PSRAM initialization, ESP-Hosted SDIO startup, LVGL landscape setup, and USB HID keyboard initialization. The remaining validation gap is Longbridge account testing with OAuth and quote permissions enabled.

## Hardware And Toolchain

- M5Stack Tab5, ESP32-P4 target
- M5Stack Tab5 keyboard kit
- ESP-IDF 5.5.x, tested with ESP-IDF 5.5.4
- Espressif `m5stack_tab5` BSP from the IDF component registry
- LVGL 9 through `esp_lvgl_port`

The checked-in `sdkconfig.defaults` carries the hardware-sensitive defaults found during Tab5 testing:

- ESP32-P4 rev v1.3 boot support with `CONFIG_ESP32P4_REV_MIN_100=y`
- 32 MB PSRAM enabled at 200 MHz
- ESP-Hosted `ESP32P4_TAB5_C6_BOARD` SDIO pin/reset preset for the on-board ESP32-C6
- ESP-Hosted transport buffers and hosted task stacks placed in PSRAM where supported
- LVGL rotated to landscape at runtime, yielding a 1280x720 application surface

Build and flash from an ESP-IDF shell:

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The repository does not include `sdkconfig`; local generated config and build outputs are ignored.
`dependencies.lock` is committed so managed component versions are reproducible. The project manifest constrains IDF to `>=5.5,<6.0` and pins `esp_lvgl_port` to `2.8.0~1`, matching the current Tab5 BSP/LVGL 9 build on ESP32-P4.

## Device Setup

On first boot the setup screen collects:

- Wi-Fi SSID and password
- Longbridge endpoint region: `global` or `cn`
- Longbridge OAuth `client_id`
- OAuth callback URI, defaulting to `http://tab5-stock.local/oauth/callback`

After saving setup, press OAuth to display the authorization URL/QR. The firmware starts a local callback server on port 80 at `/oauth/callback`, validates the OAuth `state`, exchanges the authorization code with PKCE, and stores the returned access and refresh tokens in NVS.

If Longbridge rejects the LAN callback URI policy for your app, use a temporary callback helper only for authorization-code capture. Quote traffic must remain direct from Tab5 to Longbridge.

Changing the Longbridge endpoint, OAuth `client_id`, or callback URI clears stored OAuth tokens and requires login again. Use the keyboard-focusable Reset control to factory-reset Wi-Fi, OAuth tokens, endpoint choice, and watchlist settings.

The default endpoints follow the current Longbridge documentation:

- Global HTTP API: `https://openapi.longbridge.com`
- Global quote WebSocket: `wss://openapi-quote.longbridge.com`
- China HTTP API: `https://openapi.longbridge.cn`
- China quote WebSocket: `wss://openapi-quote.longbridge.cn`

## Symbols

Symbols use Longbridge-style `ticker.region` format:

- `AAPL.US`
- `700.HK`
- `600519.SH`
- `000001.SZ`
- US tickers with embedded dots are parsed using the last region separator, for example `BRK.B.US`

The watchlist caps at 500 symbols to stay aligned with Longbridge quote subscription limits.

## Safety

- Never commit access tokens, refresh tokens, Wi-Fi passwords, App Secrets, or generated `sdkconfig` files.
- Use OAuth PKCE rather than embedding Longbridge secrets in firmware.
- NVS encryption is not enabled by default because the HMAC-backed ESP-IDF scheme can permanently burn an eFuse key on first boot. Enable it deliberately with `idf.py menuconfig` only after choosing the desired key-protection policy for your hardware.
- The v1 scope is read-only quote data. It intentionally excludes accounts, positions, orders, and trading.
- Market availability depends on the Longbridge account's OpenAPI and quote permissions.

## Tests

Portable host tests cover symbol normalization, watchlist behavior, quote merge semantics, PKCE, OAuth callback parsing, protocol frame boundaries, socket quote-pull protobuf fixtures, push quote protobuf fixtures, and reconnect backoff:

```bash
cmake -S tests/host -B build/host-tests
cmake --build build/host-tests
build/host-tests/host_tests
```

## Longbridge Push Codec

`components/longbridge/protocol.*` implements Longbridge v1 request/response/push framing. `components/longbridge/quote_codec.*` implements the v1 AUTH payload, `MultiSecurityRequest`, `SecurityQuoteResponse`, `SubscribeRequest`, `UnsubscribeRequest`, and `PushQuote` fields needed for quote-only terminal mode.

Current validation gap:

1. Confirm AUTH, `QuotePull`, subscribe, and push behavior against a real Longbridge account and Tab5 network stack.
2. Capture hardware fixtures for socket response payloads, push messages, malformed packets, permission failures, and rate limits.
3. Verify market-permission behavior for US, HK, SH, and SZ quote access on the target account.

See `docs/architecture.md` for the boundary rationale.
