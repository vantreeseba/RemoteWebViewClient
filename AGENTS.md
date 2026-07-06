# AGENTS.md — Remote WebView Client

## Project Overview

Remote WebView Client is the display-side counterpart of
[RemoteWebViewServer](https://github.com/strange-v/RemoteWebViewServer). It is
an **ESPHome external component** (`remote_webview`) for ESP32-based touch
displays: it connects to the server over WebSocket, receives JPEG image tiles
via a custom binary protocol (v1), decodes them to RGB565, and draws them
through ESPHome's `display` component while forwarding touch input back to the
server. The repo also contains `web_client/`, a browser-based test client that
speaks the same protocol for development without reflashing hardware.

## Tech Stack

| Layer       | Technology                                                       |
| ----------- | ---------------------------------------------------------------- |
| Firmware    | C++ ESPHome external component on ESP-IDF (FreeRTOS tasks/queues/semaphores) |
| Codegen     | Python ESPHome config schema + codegen (`components/remote_webview/__init__.py`) |
| Transport   | `espressif/esp_websocket_client`, custom binary protocol (v1, little-endian) |
| JPEG decode | Software: `JPEGDEC` (use the `strange-v/jpegdec-esphome` fork); Hardware: `esp_driver_jpeg` on ESP32-P4 only, with software fallback |
| Display     | ESPHome `display::draw_pixels_at` (RGB565, configurable endianness), `touchscreen`, `text_sensor` |
| Test client | TypeScript + Vite (`web_client/`), zero runtime deps              |

## Project Structure

```
RemoteWebViewClient/
├── components/remote_webview/
│   ├── __init__.py               # ESPHome config schema, validation, codegen (to_code)
│   ├── remote_webview.h/.cpp     # Component: WS tasks, reassembly, decode, draw, touch, sensors
│   ├── protocol.h                # Binary protocol v1: packed structs, parse/build helpers
│   ├── automation.h              # on_frame_update trigger + trigger_on_frame_update action
│   ├── remote_webview_config.h   # Compile-time tunables (task stacks, queue depth, move coalescing)
│   └── idf_component.yml         # IDF deps: esp_websocket_client, esp-dsp, jpegdec (+P4-only jpeg/mm)
├── web_client/                   # Browser test client (Vite + TS): protocol.ts, wsClient.ts, canvasRenderer.ts
├── examples/                     # ESPHome YAML automation examples (dimmer, URL recovery, inactivity)
└── README.md                     # Full device YAML example + Supported Parameters table
```

## Architecture Notes

- **Config flow**: YAML options are validated in `__init__.py`, applied via
  `set_*` setters, and serialized into the WS connect URI by `build_ws_uri_()`
  as query params (`id`, `w`, `h`, `r`, `ts`, `fftc`, `ffat`, `ffe`, `enf`,
  `mfi`, `q`, `mbpm`) — the same param names the server parses (in its
  `config.ts`, except `id`, which the server reads in `src/index.ts`).
  Unset numeric options stay at the `-1` sentinel and are **omitted** from the
  URI (`append_q_int_` skips `v < 0`) so server-side defaults apply — with one
  exception: `rotation_` defaults to `0`, not `-1`, so `r=0` is always sent
  even when `rotation` is absent from the YAML.
- **Two FreeRTOS tasks** (`remote_webview.cpp`): `rwv_ws` (core 0) drains the
  outbound packet queue (`q_send_`: touch, frame stats), sends a keepalive
  every 60s, restarts the client promptly when the server closes cleanly
  (the CLOSED handler sets a flag — never call stop() from the event task),
  and otherwise force-restarts only after 30s of continuous disconnection
  (routine drops are the client library's auto-reconnect);
  `rwv_decode` (core 1) drains `q_decode_` and does all JPEG decoding and
  drawing. The WS event handler reassembles fragmented binary messages into
  buffers recycled through a pre-allocated pool (`q_free_`, PSRAM-first) and
  queues a `WsMsg`; when the decode queue is full the oldest queued packet is
  evicted in favor of the newest — the stream must never block.
- **Main-loop marshaling**: the decode task never touches ESPHome objects that
  aren't thread-safe. It sets atomics (`frame_update_pending_`,
  `url_publish_pending_` + `pending_url_` under `state_mtx_`), and `loop()`
  fires the `on_frame_update` trigger (throttled to 1/sec) and publishes the
  current-URL text sensor on the main thread.
- **JPEG decode** (`decode_jpeg_tile_to_lcd_`): on ESP32-P4 the hardware
  decoder is tried first and falls back to software `JPEGDEC` on any failure,
  size mismatch, or non-16-aligned width. Everything draws through
  `display_->draw_pixels_at(...)` as RGB565; `big_endian` controls pixel byte
  order (wrong/tinted colors ⇒ flip it).
- **Touch** (`RemoteWebViewTouchListener`): Down/Move/Up map to Touch packets;
  Move events are coalesced to 60 Hz (`cfg::coalesce_moves`,
  `cfg::move_rate_hz` in `remote_webview_config.h`).
- **Binary protocol** (`protocol.h`): little-endian, `kProtocolVersion = 1`,
  packed structs each guarded by a `static_assert` on wire size. Client →
  server: Touch, Keepalive, FrameStats (reply), OpenURL. Server → client:
  Frame (tiled), FrameStats (request), CurrentURL. This file mirrors the
  server's `src/protocol.ts` — any change must be made in both repos and
  breaking changes must bump the version on both sides.
- **Web client** (`web_client/src/protocol.ts` etc.) re-implements most of the
  protocol in TypeScript: it builds Touch, OpenURL, and Keepalive packets and
  parses Frame and CurrentURL, but it does **not** send the FrameStats reply
  the ESP client sends — FrameStats round-trips can't be tested there. Keep it
  in sync too when the protocol changes; it's the fastest way to test protocol
  work without flashing a device.

## Build / Dev / Test Commands

There is no standalone firmware build — the component compiles inside an
ESPHome build that references it:

```bash
# Compile against a device YAML (see README for a full Guition 4848S040 example)
esphome compile your-device.yaml

# During local development point external_components at this checkout.
# NOTE: a bare local source resolves relative to the device YAML's directory,
# so `source: components` only works for a YAML in the repo root — from
# anywhere else, use the path to this checkout's components/ dir:
# external_components:
#   - source: components          # or /path/to/RemoteWebViewClient/components
#     components: [ remote_webview ]
```

The device YAML must register the framework components the README shows:
`espressif/esp_websocket_client` and the `strange-v/jpegdec-esphome` fork of
`bitbank2/jpegdec`. `idf_component.yml` additionally pulls `esp-dsp`, and on
ESP32-P4 only, `esp_driver_jpeg` + `esp_mm`. Set `url: "self-test"` in the
`remote_webview:` block to run the server's render-time self-test.

```bash
# Browser test client (no hardware needed)
cd web_client
npm install
npm run dev              # Vite on port 5173, host: true
npm run build            # tsc -b && vite build
```

There is no automated test suite; verification is compiling a device YAML
and/or exercising the protocol through `web_client`.

## Code Style Guidelines

### C++ (ESPHome conventions)
- Members and private methods take a trailing underscore (`ws_client_`,
  `process_packet_`, `ws_send_touch_event_`); setters are `set_x(...)`;
  snake_case is the norm. Known exceptions: private `static constexpr`
  constants are k-prefixed camelCase with no underscore (`kCoalesceMoves`,
  `kMoveRateHz`), and fields of plain nested structs (`WsMsg::buf`,
  `WsReasm::total`) carry no underscore — don't "fix" these. Static
  task-entry trampolines are suffixed `_tramp_`.
- Logging uses `ESP_LOGx(TAG, ...)` with `TAG = "Remote_WebView"`; WS
  connection-lifecycle messages (connected/disconnected/error/keepalive)
  carry a `[ws]` prefix, though other logs in the WS data path don't.
  `dump_config()` skips unset options via a `>= 0` check against the `-1`
  sentinel — but `big_endian` (default `true`) and `rotation` (default `0`)
  always pass it and print regardless of what the user set.
- **Never crash on bad input**: parsers in `protocol.h` return `false` on
  malformed/short packets; every handler bounds-checks (`off + dlen > len`)
  before touching data and drops the packet instead of aborting. Packet
  handlers drop malformed input silently (a bare `return`); `ESP_LOGW/E` on
  drop happens only in the WS event handler (oversize message, bad fragment,
  full queue) and for unknown packet types. Keep new packet handlers to this
  pattern.
- Heap allocations prefer PSRAM and fall back to internal RAM:
  `heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` then retry with
  `MALLOC_CAP_8BIT`.
- Small fixed-size packets (touch, frame stats) go through
  `queue_ws_packet_` and are sent by the WS task; the remaining direct
  senders (`ws_send_keepalive_`, `ws_send_open_url_`) guard on null
  client/mutex and connection state, take `ws_send_mtx_` with a short
  `pdMS_TO_TICKS` timeout, and return `false` on contention — sends must
  never block the decode task or main loop.
- Compile-time tunables for **new code** go in `remote_webview_config.h`
  under `namespace cfg`, not as magic numbers in the .cpp. The existing .cpp
  still hardcodes several (task priorities, reconnect/send timeouts, the 5s
  supervise loop, the OpenURL size cap) — don't launch a refactor over them.
- Unset numeric config uses `-1` as the "not set, let the server default"
  sentinel — preserve this for new options so URI building stays consistent.

### Configuration / Python
- A new YAML option needs all the layers: a `CONF_*` constant + schema entry +
  `to_code` setter in `__init__.py`, a `set_*` setter and member in
  `remote_webview.h`, usually a query param in `build_ws_uri_()` (coordinate
  the short param name with the server's `config.ts`), and a row in the
  README "Supported Parameters" table.

### Protocol Changes
- `protocol.h` structs are hand-packed with `static_assert`ed wire sizes and
  layout comments (`// [type:1][ver:1]... => N bytes`) — update the comment,
  the struct, and the assert together.
- Keep three implementations in sync: `components/remote_webview/protocol.h`,
  `web_client/src/protocol.ts`, and the server repo's `src/protocol.ts`. For
  breaking changes bump all three version constants — `kProtocolVersion`, the
  server's `PROTOCOL_VERSION`, and the web client's `PROTOCOL_VERSION`
  (`web_client/src/protocol.ts`) — the web client silently drops every frame
  whose version byte doesn't match its own constant.
