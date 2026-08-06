# Upstream-session robustness — design & decisions

**Status:** implemented (proxy side). Covers `src/upstream_session.{c,h}`,
`src/proto_handler.{c,h}`, `src/router.c`, `src/main.c`.
**Origin:** the `toPhoneQueue` stall diagnosed in [`tophone-queue-stall.md`](./tophone-queue-stall.md).

---

## 1. Problem, in one paragraph

The proxy holds a **single** config session with the node over UART (it issues
its own `want_config` at boot, caches the FromRadio burst, and replays it per
phone). The node's PhoneAPI session drains its `toPhoneQueue` **only** while in
`STATE_SEND_PACKETS`; once it falls back to `STATE_SEND_NOTHING` the queue fills
and never drains, and **only** a fresh `ToRadio{want_config}` can revive it. The
proxy used to send that exactly once, at boot, with no retry and no re-issue — so
any event that closed the node's session stranded every phone until the proxy was
power-cycled. This document records the mechanisms added to make the proxy
recover on its own, and the design decisions behind them.

## 2. Session-death modes and what recovers each

| Death mode | Trigger | Recovery mechanism |
|---|---|---|
| `ToRadio{disconnect}` forwarded | a phone app closes its session | **§3.1 disconnect absorption** (root cause, confirmed on hardware) |
| Boot-order race | proxy boots before the node's serial API exists → first `want_config` lost | **§3.3 fetch-retry watchdog** |
| Node reboot | brownout, watchdog, OTA, config write | **§3.4 liveness watchdog** (and, when viable, the `rebooted` signal — not yet implemented) |
| 15-min serial timeout | keepalive starved (repeated TX-queue-full) | **§3.4 liveness watchdog** |
| UART frame desync | a lost byte swallows whole frames incl. `config_complete` | fetch-retry watchdog (partial); full fix is RX robustness — not yet implemented |

## 3. Mechanisms

### 3.1 Absorb `ToRadio{disconnect}` (never forward it)

`proto_decode_toradio` exposes `has_disconnect`; `on_toradio_ble`
(`src/main.c`) absorbs it locally and returns, mirroring the existing
`want_config` / `heartbeat` handling. Forwarding it would call the node's
`PhoneAPI::close()` and kill the session for **every** phone.

This is the confirmed root cause of the observed incident: the node log shows
`Disconnect from phone → PhoneAPI::close()` followed by permanent
`ToPhone queue is full`, with no `STATE_SEND_PACKETS` and no `want_config`
afterward.

### 3.2 Recovery re-fetch primitive (`UPSTREAM_REFETCHING`)

`upstream_refetch()` reopens the node session **without disturbing the cache**.
A new state `UPSTREAM_REFETCHING` sits alongside `FETCHING`: frames are consumed
(swallowed, not broadcast) but **not** cached, and the terminating
`config_complete_id` returns straight to `LIVE`. The existing cache stays valid
and servable throughout, so a phone that connects mid-refetch is served from it
rather than parked `PENDING` (`main.c` serves `want_config` from cache in
`CACHE_READY | LIVE | REFETCHING`; `router.c` routes frames through
`upstream_on_fromradio` in `FETCHING | REFETCHING`).

`upstream_refetch()` is a no-op if a fetch is already in progress, so it can
never stomp the boot fetch's nonce.

### 3.3 Fetch-retry watchdog (no-progress, not a deadline)

Both the boot fetch and the recovery re-fetch are wrapped by
`s_fetch_retry_work` (`UPSTREAM_FETCH_RETRY_MS = 2000`). It is a **no-progress
watchdog**, not a completion deadline: it is rescheduled on every consumed burst
frame and fires only after 2 s of fetch **silence**. On fire it resends
`want_config` with the **same in-flight nonce** and reschedules.

Two decisions matter here:

- **No-progress, not fixed deadline.** A fixed completion deadline shorter than a
  real burst would resend mid-burst, restarting it, and under load could livelock
  (every burst interrupted before it completes). Resetting on progress makes the
  watchdog measure the inter-frame gap (sub-100 ms in a healthy burst), so a 2 s
  interval reliably distinguishes "stalled/lost" from "in progress" — and catches
  an absent node (no frames at all) quickly.
- **Same nonce across resends.** The node's `config_complete_id` echoes the nonce
  of the `want_config` that produced it. A new nonce per attempt would cause our
  own completion to be rejected (stale nonce) and loop forever. The nonce is
  chosen once per fetch and reused by every retry.
- **Cache reset only on a boot-fetch resend.** A boot resend calls `cache_begin()`
  + `phase0_reset()` first, so a restarted burst never appends to a partial one.
  A recovery resend does not cache at all.

### 3.4 Liveness watchdog (detects a dead session during `LIVE`)

`s_liveness_work` (`UPSTREAM_LIVENESS_TIMEOUT_MS = 30 min`) fires when no
**genuine** FromRadio has arrived for the timeout while `LIVE`, and triggers
`upstream_refetch()`. It is rescheduled (`upstream_liveness_kick()`) from
`router.c` on every dispatched FromRadio **except** the `queueStatus` the node
sends purely in reply to our keepalive heartbeat.

**Why exclude the keepalive `queueStatus`.** The node answers a heartbeat with a
`queueStatus` *before* the `if(!available())` guard in `getFromRadio()`, so it
replies **even from a dead (`STATE_SEND_NOTHING`) session**. Counting that reply
as proof of life would keep the liveness timer alive forever on a corpse — the
exact false-positive that makes the dead state self-perpetuating. Every other
FromRadio (mesh packets, node-originated broadcasts, a genuine `queueStatus` in
reply to a phone packet) only flows from `STATE_SEND_PACKETS`, so its presence is
valid proof and its **absence** is what we detect. The keepalive reply is already
isolated by `upstream_swallow_live_queuestatus()`; the liveness kick is placed
*after* that early return.

### 3.5 Reboot signal (`FromRadio{rebooted}`)

When the node reboots it emits `FromRadio{rebooted=true}` at serial-API init.
`router.c` detects that variant in `LIVE` and calls `upstream_refetch()`
immediately, then **absorbs** it (not broadcast — the proxy masks the reboot from
phones and refreshes the cache via the refetch). This is the *fast* reboot-recovery
path: seconds, versus up to N (§4) for the liveness watchdog.

Confirmed viable on this rig: the node runs `SerialModule` in **PROTO** mode
(`configure_params.py`: `SERIAL_MODULE_MODE=PROTO`, TXD 15 / RXD 35). That matters
because `StreamAPI::canWrite` is true at init there and `SerialModule` calls
`emitRebooted()` → the frame reaches the wire. The `canWrite`-gated dead-`emitRebooted`
defect (D1 in `tophone-queue-stall.md`) is **`SerialConsole`-specific and does not
apply** to this transport.

## 4. The N decision (`UPSTREAM_LIVENESS_TIMEOUT_MS`)

N is the single tuning knob of the liveness watchdog and it is a
**continuity-of-operation decision**, not an incidental constant:

- **Lower bound** — N must exceed the longest *legitimate* mesh silence, which is
  dominated by the telemetry interval. The node also self-generates
  telemetry/nodeinfo periodically (which reaches the proxy as FromRadio), so even
  a node with no neighbours feeds the timer; N must exceed that self-generation
  period. Too small → spurious refetches on a quiet mesh.
- **Upper bound = cost** — N *is* the worst-case window a dead session can persist
  before the proxy self-heals. With N = 30 min, phones can miss up to ~30 min of
  traffic after an unsignalled session death.
- **Default: 30 min**, chosen against a telemetry cadence on the order of minutes.
  Re-tune from the real deployment: read the node's telemetry/nodeinfo period and
  keep N comfortably above the longest expected quiet stretch.
- **Relaxation lever** — with the `rebooted` signal now handled (§3.5, reboot
  detected in seconds), the liveness watchdog is a *backstop* for the rare
  unsignalled cases (timeout, frame desync), so N can be raised further.

**No phones-connected gate.** The watchdog runs in `LIVE` regardless of whether
any phone is connected. Gating on connection count would force an `upstream_session`
→ `ble_gatt` dependency, which the module deliberately does not have. The cost of
not gating is one benign refetch per N on a fully idle mesh: a refetch with no
phones simply reopens the node session (leaving it drained and ready) and is
invisible to any connected phone (served from cache; live packets still broadcast).

## 5. Timers at a glance

| Constant | Value | Purpose | Reset / fires |
|---|---|---|---|
| `UPSTREAM_KEEPALIVE_MS` | 5 min | keep the node's 15-min serial timer alive | rescheduled on every real ToRadio TX |
| `UPSTREAM_KEEPALIVE_RETRY_MS` | 20 s | retry a dropped keepalive early (TX-queue-full) so repeated drops cannot span the 15-min timeout | on keepalive tx failure |
| `UPSTREAM_FETCH_RETRY_MS` | 2 s | resend `want_config` if a fetch stalls | reset on burst progress; fires on fetch silence |
| `UPSTREAM_LIVENESS_TIMEOUT_MS` | 30 min | detect a dead session during `LIVE` | reset on genuine FromRadio; fires on session silence |

All run on the single Zephyr system work queue (single-writer, no mutexes).
The keepalive and its `queueStatus` reply are **not** a liveness signal (§3.4).

RX-side hardening lives in `uart_meshtastic.c` (overrun accounting, a larger RX
ring, and a 250 ms mid-frame resync watchdog) — see `tophone-queue-stall.md` §D3;
it protects the frame stream that feeds everything above.

## 6. Not yet implemented / deferred

- **Per-phone slot teardown on `ToRadio{disconnect}`** — a phone that ends its API
  session without dropping the BLE link leaks its proxy slot. Parked pending the
  app-developer contract question in [`disconnect-flow.md`](./disconnect-flow.md).
- **Bulk RX drain** — the RX ring is still drained one byte per `ring_buf_get`.
  Purely a performance/starvation improvement, not a correctness gap.
