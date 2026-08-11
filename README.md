# Meshtastic BLE Proxy — nRF52840 / Zephyr

A Zephyr firmware for the **Nordic nRF52840** that acts as a **Meshtastic-compatible
BLE peripheral** and **multiplexes up to 6 phones onto a single Meshtastic node** over
UART. Where stock Meshtastic allows only one phone ↔ one node BLE link at a time, this
proxy gives each phone a connection that behaves like a standalone 1:1 link.

## What it does

- **BLE peripheral** exposing the Meshtastic GATT service (`FROMNUM`, `FROMRADIO`,
  `TORADIO`, `LOGRADIO`, plus a proxy-specific `NODE_REG`); up to
  **`CONFIG_BT_MAX_CONN` = 6 simultaneous** phone connections.
- **Per-phone config-session virtualization:** the proxy issues its own `want_config`
  to the node at boot, caches the FromRadio burst, and **replays it to each phone**
  with that phone's nonce echoed in `config_complete_id` (serve-on-read, O(1) RAM).
  Handles the app's special `want_config` nonces (`69420` config-only / `69421`
  nodes-only) via per-nonce segmentation.
- **ToRadio absorption** — three variants never reach the node, because the node's
  PhoneAPI session is shared by every phone:
  `want_config` (would restart the global config session), `heartbeat` (answered with
  a synthesized `queueStatus`), and `disconnect` (would put the node in
  `STATE_SEND_NOTHING` and stall `toPhoneQueue` for everyone — it instead tears down
  just that phone's slot). Everything else is forwarded verbatim.
- **FromRadio → phones:** broadcast to all connections, or **targeted** by a custom
  proxy header (`PROXY_PORTNUM 256` + 4-byte `DST_ID`) for per-phone addressing.
  Unroutable frames fall back to broadcast — **never a silent drop**.
- **Self-healing upstream session:** UART keepalive (~5 min idle, 20 s retry on TX
  failure), a **no-progress fetch-retry watchdog** (2 s of fetch silence → resend
  `want_config` with the *same* nonce), a **liveness watchdog** (30 min without a
  genuine FromRadio → recovery re-fetch), and fast reboot recovery from
  `FromRadio{rebooted}`. Recovery re-fetches consume the burst without touching the
  cache, so phones stay servable throughout.
- **Hardened UART RX:** 2 KB ring with overrun accounting, 2 ms hardware-async idle
  flush, and mid-frame resync after 250 ms of stall.
- **Debug affordances:** compile-out-able per-hop `ROUTE UP/DN` tracing and activity
  LEDs (BLE FromRadio/ToRadio, UART RX/TX).

## Hardware

- **Board:** nRF52840 DK (`boards/nrf52840dk_nrf52840.overlay`).
- **UART1 → Meshtastic node:** `P1.01` RX / `P1.02` TX, 115200 8N1 (Stream API framing
  `0x94 0xC3 len_hi len_lo …`).
- **UART0:** JLink RTT logging (independent of the mesh link).
- **Node side:** a LILYGO T-Beam running `SerialModule` in **PROTO** mode with its own
  Bluetooth disabled — see [`tools/meshtastic/`](tools/meshtastic/README.md).

## Build (nRF Connect SDK / Zephyr)

Built and tested against **NCS v2.7.0** (Zephyr + nanopb).

### Protobuf dependency (REQUIRED before building)

The build generates nanopb C sources from the **Meshtastic `.proto` definitions**,
expected at **`./proto/meshtastic/`**. They are **not** included in this repo (`proto/`
is git-ignored so a machine-specific symlink is never committed) — provide them first:

```sh
git submodule add --force https://github.com/meshtastic/protobufs proto
```

`CMakeLists.txt` reads these files from `proto/meshtastic/`:
`mesh`, `portnums`, `channel`, `config`, `device_ui`, `module_config`, `atak`,
`telemetry`, `xmodem` (`.proto`).

### Configure & flash

```sh
west build -b nrf52840dk_nrf52840
west flash
```

Set the BLE device name in `prj.conf` before flashing — it must match
`^.*_([0-9a-fA-F]{4})$` (a 4-hex suffix), e.g. `Meshtastic_CA1E`.

### Configuration knobs

| Setting | Where | Notes |
|---|---|---|
| `CONFIG_BT_DEVICE_NAME` | `prj.conf` | Must carry the 4-hex suffix; per-deployment |
| `CONFIG_BT_MAX_CONN` | `prj.conf` | Sizes both the BT stack and `MAX_BLE_CONNECTIONS` |
| `CONFIG_MESHTASTIC_ROUTE_TRACE` | `Kconfig` | Per-hop path logs; **off for production** (compiles out) |
| `CONFIG_MESHTASTIC_ROUTE_TRACE_PAYLOAD` | `Kconfig` | Also hexdumps payloads — reveals message text |
| `CONFIG_UART_1_NRF_HW_ASYNC` + `_TIMER=2` | `prj.conf` | Required: flushes partial DMA buffers on the idle gap |

Timing constants (keepalive, fetch retry, liveness timeout) live at the top of
`src/upstream_session.c`; the rationale for each is in
[`docs/upstream-session-robustness.md`](docs/upstream-session-robustness.md).

## Source layout (`src/`)

| File | Responsibility |
|---|---|
| `main.c` | Boot order; ToRadio dispatch (want_config / heartbeat / disconnect handled locally, packets → UART) |
| `ble_gatt.c/.h` | GATT service, per-phone state machine, serve-on-read replay, queueStatus reply, slot teardown |
| `uart_meshtastic.c/.h` | UART1 async/DMA driver, Stream API framing, RX ring + resync, TX queue |
| `proto_handler.c/.h` | nanopb decode (FromRadio/ToRadio) + encoders (config_complete / heartbeat / queueStatus) |
| `proxy_protocol.c/.h` | Custom proxy header (VERSION / 4-byte SRC / 4-byte DST / content), `PROXY_PORTNUM 256` |
| `router.c/.h` | FromRadio dispatch: fetch→cache, LIVE→broadcast / targeted; keepalive swallow, reboot signal, liveness kick |
| `config_cache.c/.h` | Packed arena + index of the boot burst; per-nonce segmentation; atomic ready barrier |
| `upstream_session.c/.h` | Boot `want_config`, `BOOT→FETCHING→CACHE_READY→LIVE` (+ `REFETCHING`), keepalive and watchdogs |
| `route_trace.h` | `ROUTE_TRACE` macros, gated by `CONFIG_MESHTASTIC_ROUTE_TRACE` |

## Documentation

| Doc | Contents |
|---|---|
| [`architecture.md`](docs/architecture.md) | Data flow, router decision tree, state machines, `want_config` sequence, threading model and priorities, boot order, invariants |
| [`client-integration.md`](docs/client-integration.md) | **Start here for app teams** — GATT UUIDs, connection lifecycle, wire format, the four app changes, gotchas |
| [`fromradio-routing.md`](docs/fromradio-routing.md) | Per-variant disposition table (cache / broadcast / unicast / absorb) |
| [`portnums.md`](docs/portnums.md) | Meshtastic `PortNum` reference and what the proxy routes on |
| [`upstream-session-robustness.md`](docs/upstream-session-robustness.md) | Session-death modes and each recovery mechanism (implemented) |
| [`tophone-queue-stall.md`](docs/tophone-queue-stall.md) | Root-cause investigation of the node-side `toPhoneQueue` stall |
| [`disconnect-flow.md`](docs/disconnect-flow.md) | Proxy ↔ app disconnect contract (open question for the app side) |
| [`reconnect-persistence.md`](docs/reconnect-persistence.md) | Design proposal — not implemented |
| [`seq-numbers-pdr.md`](docs/seq-numbers-pdr.md) | Design proposal — wire-format v2 with per-pair sequence numbers, not implemented |

Host-side node provisioning and health-check scripts:
[`tools/meshtastic/`](tools/meshtastic/README.md).

## Phone-app integration (summary)

- **Stock Meshtastic app:** works as-is via **broadcast** — every phone behaves like a
  client of the same node (shared `nodenum`, shared config).
- **Per-phone addressing (router):** a custom/modified app registers its `proxy_id`
  (NODE_REG), frames messages with the proxy header inside a `portnum 256` MeshPacket,
  and maintains a `proxy_id → nodenum` directory. Unregistered `DST_ID`s fall back to
  broadcast.

**→ Full integration guide for app teams: [`docs/client-integration.md`](docs/client-integration.md)**

## Status

**v1.0** — per-phone config virtualization + segmentation + heartbeats; validated on
hardware with 3 concurrent phones (full config, mesh messaging + ACKs).

**v1.1 (in progress)** — upstream-session robustness has landed: `disconnect`
absorption, recovery re-fetch, fetch-retry and liveness watchdogs, `rebooted` handling,
UART RX hardening, and per-phone slot teardown.

Still open:

- **Hardware watchdog (WDT)** — deferred.
- **Cumulative stats counters** (`src/pbx_stats.c/.h`) — written but **not yet wired**:
  absent from `CMakeLists.txt`, `CONFIG_MESHTASTIC_STATS_PERIOD_S` is not declared in
  `Kconfig`, and no module calls `pbx_stats_inc()` / `pbx_stats_start()` yet.
- Design proposals not implemented: reconnect persistence, wire-format v2 sequence
  numbers.

## License

TBD. Note the Meshtastic protobuf definitions pulled in at build time are separately
licensed by the Meshtastic project.
