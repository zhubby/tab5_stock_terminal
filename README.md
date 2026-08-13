# Tab5 Longbridge Stock Terminal

Read-only stock quote terminal for M5Stack Tab5 with keyboard kit. The firmware is an ESP-IDF application that stores credentials on-device, connects directly to Longbridge OpenAPI, and renders a watchlist-first quote dashboard for US, Hong Kong, Shanghai, and Shenzhen symbols.

## Status

This repo now contains the ESP-IDF project scaffold, domain model, settings persistence, OAuth PKCE flow, API Key auth, local OAuth callback server, Longbridge socket-token client, Longbridge quote WebSocket client, Longbridge v1 frame/quote protobuf payload codec, LVGL terminal UI, and host unit tests for the portable core logic.

The runtime path opens the quote WebSocket with `/v1/socket/token`, sends AUTH, pulls full snapshots with the socket `QuotePull` command, subscribes with `SubType.Quote`, and merges sparse `PushQuoteData` updates into the local quote store. Real Tab5 boot validation has passed through PSRAM initialization, ESP-Hosted SDIO startup, LVGL landscape setup, and USB HID keyboard initialization. The remaining validation gap is Longbridge account testing with OAuth or API Key credentials and quote permissions enabled.

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
- Auth mode: `OAuth` or `API key`
- OAuth mode: Longbridge OAuth `client_id` and callback URI
- API Key mode: Longbridge `app_key`, `app_secret`, and `access_token`

Alternatively, place `TAB5.CFG` or `TAB5.INI` at the root of the Tab5 microSD card. On boot the firmware mounts the SD card, reads the first matching file from `/sdcard`, stores the imported settings in NVS, unmounts the card, and skips the setup form when Wi-Fi, quote auth credentials, and watchlist are present. Long filenames `tab5_stock_terminal.conf` and `tab5_stock_terminal.ini` are also checked when FATFS long-filename support is enabled, but the 8.3 names are the safest choice on the current firmware defaults.

Example SD card config:

```ini
# /sdcard/TAB5.CFG
wifi_ssid=Office Wi-Fi
wifi_password=your-wifi-password
endpoint=global
client_id=your-longbridge-client-id
redirect_uri=http://tab5-stock.local/oauth/callback
watchlist=AAPL.US,700.HK,600519.SH,000001.SZ
```

API Key SD card config:

```ini
# /sdcard/TAB5.CFG
wifi_ssid=Office Wi-Fi
wifi_password=your-wifi-password
endpoint=global
auth_mode=api_key
app_key=your-longbridge-app-key
app_secret=your-longbridge-app-secret
access_token=your-longbridge-access-token
watchlist=AAPL.US,700.HK,600519.SH,000001.SZ
```

Supported keys:

- `wifi_ssid` or `ssid`
- `wifi_password`, `wifi_pass`, or `password`
- `endpoint`, `endpoint_region`, or `longbridge_endpoint`: `global` or `cn`
- `auth_mode`, `auth`, or `longbridge_auth`: `oauth` or `api_key`
- `client_id` or `longbridge_client_id`
- `redirect_uri`, `oauth_redirect_uri`, or `callback_uri`
- `app_key`, `longbridge_app_key`, or `api_app_key`
- `app_secret`, `longbridge_app_secret`, or `api_app_secret`
- `api_access_token`, `api_token`, or `longbridge_access_token`
- `watchlist`: comma-separated symbols
- `symbol`: one additional symbol per line
- `reset_tokens=true`: clear existing OAuth tokens while importing
- `access_token`: OAuth access token in OAuth mode, or API Key access token when `auth_mode=api_key`
- `refresh_token`, `token_expires_at`: optional OAuth token import for controlled lab use

If the SD card config does not include OAuth tokens, previously stored NVS tokens are preserved only when endpoint, auth mode, `client_id`, and callback URI still match. In API Key mode, existing NVS API Key credentials are preserved when the SD card selects API Key mode but omits the credential fields.

After saving setup, press OAuth to display the authorization URL/QR. The firmware starts a local callback server on port 80 at `/oauth/callback`, validates the OAuth `state`, exchanges the authorization code with PKCE, and stores the returned access and refresh tokens in NVS.

After Wi-Fi connects, the firmware also starts a local SD card file manager at `http://tab5-stock.local:8080/`. It exposes the Tab5 microSD card mounted at `/sdcard` for LAN browsing, text editing, new file/folder creation, downloads, and deletion. The service generates a random access token on each start; the device status line shows that the service is running, and the serial log prints the full `?token=...` URL. Text editing and scripted uploads are limited to files up to 256 KB; larger files can still be downloaded from the listing.

For scripted access, use:

```bash
TOKEN=the-token-shown-in-the-tab5-log
curl -H "X-Tab5-SD-Token: $TOKEN" http://tab5-stock.local:8080/TAB5.CFG
curl -X PUT -H "X-Tab5-SD-Token: $TOKEN" --data-binary @local.txt \
  'http://tab5-stock.local:8080/api/file?path=/notes/local.txt'
curl -X DELETE -H "X-Tab5-SD-Token: $TOKEN" \
  'http://tab5-stock.local:8080/api/file?path=/notes/local.txt'
```

`PUT /api/file?path=/absolute/sd/path` creates or replaces one file. `GET /path` views a directory in the browser or downloads a file. `DELETE /api/file?path=/absolute/sd/path` removes a file or empty folder. Paths must be absolute, URL-encoded UTF-8 paths under `/sdcard`; traversal and FAT-invalid characters are rejected. Folder creation is available from the browser UI.

In API Key mode, quote startup skips OAuth entirely. The firmware signs the `/v1/socket/token` request with the Longbridge SDK-compatible HMAC-SHA256 headers: `authorization`, `x-api-key`, `x-dc-region`, `x-timestamp`, and `x-api-signature`.

If Longbridge rejects the LAN callback URI policy for your app, use a temporary callback helper only for authorization-code capture. Quote traffic must remain direct from Tab5 to Longbridge.

Changing the Longbridge endpoint, auth mode, OAuth `client_id`, callback URI, or API Key credentials disconnects the quote stream and clears stale auth state. Use the keyboard-focusable Reset control to factory-reset Wi-Fi, OAuth tokens, API Key credentials, endpoint choice, and watchlist settings.

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
- Treat SD card config files as credentials when they include Wi-Fi passwords, OAuth tokens, API access tokens, or App Secrets.
- The SD card web file manager uses a per-start token, but it is still a plain HTTP LAN tool. Use it only on trusted networks and turn off Wi-Fi or remove the SD card when exposing sensitive files.
- OAuth PKCE avoids embedding App Secrets in firmware. API Key mode is supported for local/private devices, but stores `app_secret` on-device.
- NVS encryption is not enabled by default because the HMAC-backed ESP-IDF scheme can permanently burn an eFuse key on first boot. Enable it deliberately with `idf.py menuconfig` only after choosing the desired key-protection policy for your hardware.
- The v1 scope is read-only quote data. It intentionally excludes accounts, positions, orders, and trading.
- Market availability depends on the Longbridge account's OpenAPI and quote permissions.

## Tests

Portable host tests cover symbol normalization, watchlist behavior, SD card settings-file parsing, SD web file-manager path safety helpers, API Key signing, quote merge semantics, PKCE, OAuth callback parsing, protocol frame boundaries, socket quote-pull protobuf fixtures, push quote protobuf fixtures, and reconnect backoff:

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
