# Finding: `toPhoneQueue` fills and never drains on a serial-attached Meshtastic node

> **Language note:** this document is written in English on purpose. Per
> `CLAUDE.md` § Conversation language, portable technical records stay in English —
> this one is meant to be pasteable into a `meshtastic/firmware` issue and into the
> proxy's own `docs/`. All session/state/memory documents for this work are in Spanish.

**Date:** 2026-08-04
**Investigated by:** orchestrator session (evidence-only; no hardware run)
**Status:** root cause identified from code; awaits one hardware log to confirm the trigger

---

## 1. Setup under investigation

| Side | Device | Role |
|---|---|---|
| Proxy | Nordic **nRF52840 DK**, Zephyr (NCS v2.7.0) | `meshtastic-ble-proxy` — BLE peripheral multiplexing up to 6 phones onto one node |
| Node | **LilyGO T-Beam** (`board = ttgo-tbeam`, extends `esp32_base`) → **classic ESP32** | stock Meshtastic firmware, serial (Stream API) client |
| Link | UART, **115200 8N1, no flow control**. Proxy side: UART1, P1.01 RX / P1.02 TX | Stream API framing `0x94 0xC3 len_hi len_lo …` |

The proxy is the node's **sole** serial API client. Phones never talk to the node
directly: the proxy issues its own `want_config` at boot, caches the FromRadio burst
and replays it per phone.

## 2. Observed symptoms (reported from hardware)

1. **A session was established successfully.** Some time later, `toPhoneQueue` on the
   node filled up and **never drained again**. (The reporter notes the causality is
   probably the reverse of the intuitive one: it filled *because* it stopped draining.)
2. **Rebooting the Nordic (the proxy) restored normal operation** every time.

Both symptoms are fully explained by §3. Symptom 2 is the decisive clue: a proxy
reboot is the **only** event in this system that emits a fresh `want_config`.

## 3. Root cause

**The node's PhoneAPI session dies silently and there is no path back, while the
proxy's own liveness check keeps reporting the link as healthy.**

### 3.1 The node closes the session on inactivity

`SerialConsole::checkIsConnected()` (`src/SerialConsole.cpp:169-172`) declares the
client gone when nothing has been received for `SERIAL_CONNECTION_TIMEOUT`
(`src/SerialConsole.cpp:29`) = **15 minutes**. It is polled every iteration from
`StreamAPI::runOncePart()` → `checkConnectionTimeout()`
(`src/mesh/StreamAPI.cpp:17`, `src/mesh/PhoneAPI.cpp:420-431`), which calls
`close()`.

`close()` sets `state = STATE_SEND_NOTHING` (`src/mesh/PhoneAPI.cpp:373-374`).

### 3.2 With the session closed, the queue can never drain

- `available()` returns `false` immediately for `STATE_SEND_NOTHING`
  (`src/mesh/PhoneAPI.cpp:1646-1647`).
- `getFromRadio()` therefore returns 0 at its early exit
  (`src/mesh/PhoneAPI.cpp:566-568`).
- `toPhoneQueue` is **only ever serviced inside the `STATE_SEND_PACKETS` branch of
  `available()`** (`src/mesh/PhoneAPI.cpp:1704` — the single `service->getForPhone()`
  call site on this path). No other state touches it.

Meanwhile the producer side never stops: `MeshService::handleFromRadio()` copies and
enqueues **every** received mesh packet unconditionally
(`src/mesh/MeshService.cpp:117-118`), with no check for whether any client is
draining. The declaration itself carries a years-old acknowledgment of the hazard
(`src/mesh/MeshService.h:41-44`): *"FIXME, change to a DropOldestQueue … to ensure we
never hang because android hasn't been there in a while"*.

### 3.3 Only `want_config` can revive it — and nothing sends one

`state` leaves `STATE_SEND_NOTHING` **exclusively** via `handleStartConfig()`, which
is reached only from `ToRadio.want_config_id` (`src/mesh/PhoneAPI.cpp:466-469`).

On the proxy side:

- `upstream_session_start()` is called **once**, from `main()` (`src/main.c:161`).
  Both of its failure paths log and return leaving the state at `BOOT`, with the
  comment *"A retry policy belongs to main/QA"* (`src/upstream_session.c:306`, `:317`).
  **No retry policy exists anywhere in the proxy.**
- A phone's `want_config` is **deliberately never forwarded** to the node
  (`src/main.c:71-84`) — correct for multiplexing, but it means no phone action can
  ever re-establish the node's session.

Result: once closed, the session stays closed until the proxy is power-cycled. ⇒ **Symptom 2.**

### 3.4 The proxy's liveness check is a false positive (why nobody notices)

This is what makes the failure silent rather than detected.

The proxy's keepalive sends `ToRadio{heartbeat}` every 5 minutes
(`src/upstream_session.c:77`, `keepalive_work_handler` `:251-282`) and treats the
node's `queueStatus` reply as proof of life.

On the node, `getFromRadio()` answers a heartbeat **before** the `!available()`
guard — the `if (heartbeatReceived)` block at `src/mesh/PhoneAPI.cpp:556-564` returns
an encoded `queueStatus` and never consults `state`. And `ToRadio{heartbeat}`
handling (`src/mesh/PhoneAPI.cpp:503-521`) sets `heartbeatReceived = true` **without
calling `handleStartConfig()`**.

So after `close()`:

- the proxy's heartbeat is answered normally → **link looks healthy**;
- `lastContactMsec` is refreshed by that heartbeat → **the node never times out again**;
- but `state` remains `STATE_SEND_NOTHING` → **`toPhoneQueue` never drains**.

The system settles into a stable, self-perpetuating dead state that both sides
consider fine. ⇒ **Symptom 1.**

### 3.5 Why the 15 minutes of silence happen at all

`lastContactMsec` is refreshed only by inbound ToRadio (`src/mesh/PhoneAPI.cpp:439`).
The proxy sends upstream traffic only for (a) real phone packets and (b) the 5-minute
keepalive. Two proxy behaviors can open a >15-minute gap:

- **Keepalive suppression by phone traffic.** `upstream_keepalive_reschedule()`
  (`src/main.c:112`, `src/upstream_session.c:450-461`) resets the timer to the **full**
  5 minutes on every forwarded packet. Phone `want_config` and `heartbeat` are absorbed
  locally and do *not* reschedule (`src/main.c:83`, `:90`) — but any real packet does.
  Traffic patterns that keep pushing the timer out while producing no upstream bytes
  for long stretches are possible; each forwarded packet does refresh `lastContactMsec`,
  so this alone needs an adverse pattern.
- **Silent keepalive loss.** If `uart_meshtastic_tx()` returns `-ENOMEM` because the
  4-deep TX queue is full (`src/uart_meshtastic.c:38`, `:359-363`) — e.g. several phones
  writing at once — the keepalive is **dropped and not retried early**: the handler
  simply reschedules the full 5 minutes (`src/upstream_session.c:268-270`, `:281`).
  **Three consecutive drops = 15 minutes = the node closes the session.**

The exact trigger on this rig needs one hardware log (§6). The mechanism from §3.1
onward is independent of which of these opened the gap.

## 4. Independent defects found (each real on its own)

### D1 — `emitRebooted()` is dead code: the node can never announce a reboot

`SerialConsole`'s constructor sets `canWrite = false` (`src/SerialConsole.cpp:66`) and
then, 18 lines later, calls `emitRebooted()` (`src/SerialConsole.cpp:84`).

`emitRebooted()` → `emitTxBuffer()` → `writeFrame()`, and **both** implementations bail
out on `!canWrite` before writing a byte:

- `StreamAPI::writeFrame` — `src/mesh/StreamAPI.cpp:195`: `if (len == 0 || !canWrite) return false;`
- `SerialConsole::writeFrame` (USB-CDC path) — `src/SerialConsole.cpp:227`: same check.

So the boot-time `FromRadio{rebooted=true}` **cannot reach the wire under any build**.
`grep -rn emitRebooted src/` finds only this constructor call and one unrelated caller
in `src/modules/SerialModule.cpp:237`.

**Impact:** the one signal a serial client could use to detect "the node restarted, your
session is gone" does not exist in practice. Any client-side recovery built on
`rebooted` would silently never fire.

**Fix (node):** either move `emitRebooted()` to the point where `canWrite` first becomes
true (`SerialConsole::handleToRadio`, `src/SerialConsole.cpp:253`), or give it a
`bestEffort`/force path that bypasses the `canWrite` gate. Note the flag exists for a
good reason — don't blast unframed bytes at a port whose client hasn't identified
itself — so gating the *re-emission* on the first client contact is the safer shape.

### D2 — The proxy ignores `rebooted` entirely

`grep -rn "rebooted\|reboot" src/` in the proxy returns **zero matches**.
`proto_decode_fromradio` only distinguishes `packet_tag` from everything else
(`src/proto_handler.c:60`), and `router_dispatch` broadcasts all non-packet variants
verbatim (`src/router.c:65-73`).

Combined with D1 this is doubly dead: the node can't send it and the proxy wouldn't
act on it. Both halves need fixing for reboot-driven recovery to work.

### D3 — The proxy loses RX bytes silently on ring-buffer overrun

`src/uart_meshtastic.c:248`:

```c
ring_buf_put(&rx_ring_buf,
             evt->data.rx.buf + evt->data.rx.offset,
             evt->data.rx.len);
```

**The return value is discarded.** Zephyr's `ring_buf_put()` returns the number of bytes
actually written and writes *fewer* than requested when the buffer is full. The excess is
dropped with no log, no counter, and no error.

Quantities that make this reachable:

| Quantity | Value | Source |
|---|---|---|
| Line rate | 115200 8N1 = **11 520 B/s** | `src/uart_meshtastic.c:54` |
| RX ring buffer | **1024 B** | `src/uart_meshtastic.c:36` (`RING_BUF_SIZE`) |
| **Time to overflow the ring** | **≈ 89 ms** of unserviced RX | 1024 / 11520 |
| DMA double buffer | 2 × 256 B | `src/uart_meshtastic.c:35`, `:77` |
| Config burst duration | `total_bytes / 11520` s | measure via `TOTAL: %u B` (`src/upstream_session.c:184`) |

`rx_work_handler` (`src/uart_meshtastic.c:184-191`) drains the ring **one byte per
`ring_buf_get()` call** and runs on the **Zephyr system work queue** — the same queue
that serves BLE callbacks, the per-phone replay, the keepalive and the LED blink work.
With up to 6 connections, 89 ms of work-queue latency is not an extreme case.

**Downstream consequence:** the frame state machine `rx_sm`
(`src/uart_meshtastic.c:129-181`) has **no timeout and no resync on loss**. A byte lost
in the length field leaves it in `RX_WAIT_PAYLOAD` waiting for up to
`MESHTASTIC_MAX_PAYLOAD` bytes, so it **swallows subsequent whole frames as payload**. If
one of those is the `config_complete_id`, `upstream_session` stays `FETCHING` forever —
and per §3.3 there is no retry. This is a second, independent route into the same dead
state, and the likely explanation for *intermittent* failures after a good session.

**Fixes (proxy):**
1. Check the return of `ring_buf_put` and count/log the shortfall — an overrun must never
   be silent.
2. Give `rx_sm` an inter-frame timeout that resets to `RX_WAIT_MAGIC1`, so a loss costs
   one frame instead of an unbounded run of them.
3. Enlarge `RING_BUF_SIZE` and drain with a bulk `ring_buf_get` (claim/finish) instead of
   byte-at-a-time; consider a dedicated work queue for RX so BLE work cannot starve it.

### D4 — Node boot-order window silently discards early `ToRadio`

`SerialConsole::handleToRadio` gates *everything* on config being loaded
(`src/SerialConsole.cpp:243-259`):

```cpp
if (config.has_lora && config.security.serial_enabled) { … canWrite = true; … }
else return false;   // discarded: no NAK, no log, canWrite stays false
```

Both flags are set only when `NodeDB` is constructed —
`config.has_lora = true` (`src/mesh/NodeDB.cpp:856`) and
`config.security.serial_enabled = true` (`src/mesh/NodeDB.cpp:1020`).

The boot ordering in `src/main.cpp` leaves a wide window:

| `src/main.cpp` | Step |
|---|---|
| **421** | `consoleInit()` — **SerialConsole exists; RX state machine is live from here** |
| 454 | `powerMonInit()` |
| 477 | `initDeepSleep()` |
| 537 | `initSPI()` |
| 539 | `OSThread::setup()` |
| **541** | `fsInit()` — **filesystem mount** |
| 544 | `EncryptedStorage::initLocked()` |
| 582–604 | `Wire1.begin()` ×2, `Wire.begin()` ×2 |
| 631–634 | `power = new Power()`, `power->setup()` |
| 639 | `mcp23017EarlyInit()` |
| **679** | `i2cScanner->countDevices()` — **full I2C scan, 80+ device types** |
| 690 | `printPartitionTable()` |
| 702–765 | screen / RTC / keyboard / AQI detection |
| **854** | `nodeDB = new NodeDB` — **`has_lora` and `serial_enabled` become true only here** |

A filesystem mount plus a full I2C bus scan is not a microsecond affair. **The absolute
duration was not measured** — it must be read off the device (§6, item 4) — but the
window provably spans every step above.

The proxy reaches `upstream_session_start()` after only
`ble_gatt_init` → `bt_enable` → advertising → `uart_meshtastic_init`
(`src/main.c:123-161`), i.e. within a few hundred milliseconds of its own boot. **If both
boards share a power rail — the normal bench setup — the proxy wins this race and its
single `want_config` is silently dropped.** With no retry, the node then sits in
`STATE_SEND_NOTHING` from power-up and `toPhoneQueue` fills without ever having drained.

This is a *second* way to reach the reported end state, distinguishable from §3 by
whether a session was ever seen at all. For this rig the reporter observed a working
session first, so §3 is the operative path here — but D4 must be fixed for the system to
be robust across reboots, and it is the same one-line fix (retry).

**Fix (proxy):** retry `want_config` with backoff until state reaches `LIVE`.
**Fix (node, nice-to-have):** log the discard instead of dropping it silently.

### D5 — `close()` leaves the transport half-configured

`close()` (`src/mesh/PhoneAPI.cpp:357-418`) resets a great deal of PhoneAPI state but
does **not** touch two transport-level flags:

- `StreamAPI::canWrite` (`src/mesh/StreamAPI.h:86`) stays `true`
- `SerialConsole::usingProtobufs` (`src/SerialConsole.h:15`) stays `true`

Consequence: after a timeout close, the node still considers the port a protobuf port —
`SerialConsole::write()` keeps swallowing raw console bytes
(`src/SerialConsole.cpp:145-154`) and `log_to_serial` keeps routing through
`emitLogRecord` (`:262-274`). So the operator loses plain console output *and* gets no
API traffic. It also means `writeStream()`'s `canWrite` guard is **not** what stops the
drain in this scenario — the `available()`/state gate is.

### D6 — Queue sizing and wake-up on the node (aggravating factors, not causes)

- **`toPhoneQueue` holds 8 entries on this node.** `MAX_RX_TOPHONE` is 8 for classic
  ESP32 — `#if defined(ARCH_ESP32) && !(CONFIG_IDF_TARGET_ESP32C3 || …S3)`
  (`src/mesh/mesh-pb-constants.h:22-23`). Not 16 (nRF52840, `:24-29`) and not 32
  (RP2040/STM32WL, `:30-34`). The T-Beam has the **smallest** queue of any platform.
- **Overflow policy** (`src/mesh/MeshService.cpp:490-511`): `TEXT_MESSAGE_APP` and
  `RANGE_TEST_APP` evict the oldest; **every other portnum is dropped**. `fromNum++`
  fires in both cases, so clients are told "new data available" while data is discarded.
- **`toPhoneQueue.setReader()` is never called.** `StaticPointerQueue::enqueue` wakes a
  registered reader thread (`src/mesh/StaticPointerQueue.h:41-45`), but
  `grep -rn setReader src/` finds only `src/graphics/Screen.cpp:640` and
  `src/mesh/Router.cpp:158`. Enqueuing for the phone wakes nothing.
- **On classic ESP32 the serial thread parks itself.** `esp32-common.ini:58` defines
  `-DSERIAL_HAS_ON_RECEIVE`, and it survives (the `#undef` at
  `src/SerialConsole.cpp:13-15` only applies when `ARDUINO_USB_CDC_ON_BOOT` is set, which
  classic ESP32 does not have). So `SerialConsole::runOnce()` ends with
  (`src/SerialConsole.cpp:124-125`):

  ```cpp
  return Port.available() ? delay : INT32_MAX;
  ```

  With no inbound bytes the thread schedules itself **INT32_MAX ms out**. The two wake
  hooks — `rxInt()` (inbound UART bytes) and `onNowHasData()` — both only call
  `setIntervalFromNow(0)`, which merely sets `interval` and `_cached_next_run`
  (`src/concurrency/OSThread.cpp:51-58`); **neither calls `mainDelay.interrupt()`**, the
  way `StaticPointerQueue::enqueue` does. The thread is therefore marked due but the main
  loop is only guaranteed to notice on its next iteration
  (`src/main.cpp:1528-1543`, `mainController.runOrDelay()` → `mainDelay.delay()`).
  This costs latency rather than causing the stall, but it means **inbound UART bytes are
  the only robust way to get the serial thread running**, which is exactly why a proxy
  reboot appears to "fix" things instantly.
- **`onNotify` does not even wake the thread outside `STATE_SEND_PACKETS`**
  (`src/mesh/PhoneAPI.cpp:1880-1885`) — it logs
  `"Client not yet interested in packets (state=%d)"`.

### D7 — The config burst is a no-drain window by construction

Independently of any failure: from `want_config` until `config_complete_id`, the state
machine walks `MY_INFO → UIDATA → OWN_NODEINFO → METADATA → REGION_PRESETS → CHANNELS →
CONFIG → MODULECONFIG → OTHER_NODEINFOS → FILEMANIFEST → COMPLETE_ID` and **none of
those states touch `toPhoneQueue`** (§3.2). The whole burst is emitted inside a single
`writeStream()` call — the `do … while(len)` at `src/mesh/StreamAPI.cpp:59-64` does not
yield until `getFromRadio()` returns 0 — with a blocking `stream->flush()` per frame
(`:207`). At 11 520 B/s that window is `total_bytes / 11520` seconds, during which an
8-slot queue can only grow. Read the real figure off the proxy's own instrumentation:
`log_phase0_breakdown()`, line `TOTAL: %u B` (`src/upstream_session.c:184`).

## 5. Evidence index

| Claim | Location |
|---|---|
| Queue is 8 entries on classic ESP32 | `firmware/src/mesh/mesh-pb-constants.h:22-23` |
| T-Beam is classic ESP32 | `firmware/variants/esp32/tbeam/platformio.ini:12-14` |
| `SERIAL_HAS_ON_RECEIVE` defined for ESP32 | `firmware/variants/esp32/esp32-common.ini:58` |
| Unconditional enqueue for phone | `firmware/src/mesh/MeshService.cpp:117-118` |
| Overflow drop/evict policy | `firmware/src/mesh/MeshService.cpp:490-511` |
| Known-hazard FIXME on the queue | `firmware/src/mesh/MeshService.h:41-44` |
| Queue serviced only in `STATE_SEND_PACKETS` | `firmware/src/mesh/PhoneAPI.cpp:1704` |
| `STATE_SEND_NOTHING` ⇒ `available()==false` | `firmware/src/mesh/PhoneAPI.cpp:1646-1647` |
| `getFromRadio` early exit | `firmware/src/mesh/PhoneAPI.cpp:566-568` |
| Heartbeat answered before the state gate | `firmware/src/mesh/PhoneAPI.cpp:556-564` |
| Heartbeat does not restart the session | `firmware/src/mesh/PhoneAPI.cpp:503-521` |
| Only `want_config` sets state | `firmware/src/mesh/PhoneAPI.cpp:466-469` |
| 15-minute serial timeout | `firmware/src/SerialConsole.cpp:29`, `:169-172` |
| `close()` ⇒ `STATE_SEND_NOTHING` | `firmware/src/mesh/PhoneAPI.cpp:373-374` |
| `canWrite` gate blocks `emitRebooted` | `firmware/src/SerialConsole.cpp:66`, `:84`; `src/mesh/StreamAPI.cpp:195` |
| Boot window start / end | `firmware/src/main.cpp:421` / `:854` |
| `has_lora`, `serial_enabled` set at NodeDB | `firmware/src/mesh/NodeDB.cpp:856`, `:1020` |
| `setIntervalFromNow` does not interrupt the delay | `firmware/src/concurrency/OSThread.cpp:51-58` |
| Proxy: single `want_config`, no retry | `proxy/src/main.c:161`; `src/upstream_session.c:306`, `:317` |
| Proxy: phone `want_config` never forwarded | `proxy/src/main.c:71-84` |
| Proxy: keepalive 5 min, reschedule-on-TX | `proxy/src/upstream_session.c:77`, `:450-461`; `src/main.c:112` |
| Proxy: keepalive drop not retried early | `proxy/src/upstream_session.c:268-270`, `:281` |
| Proxy: unchecked `ring_buf_put` | `proxy/src/uart_meshtastic.c:248` |
| Proxy: ring 1024 B, DMA 2×256 B, TX depth 4 | `proxy/src/uart_meshtastic.c:35-38` |
| Proxy: no `rebooted` handling | `grep -rn "rebooted\|reboot" proxy/src/` → 0 matches |

## 6. How to confirm on hardware

Ordered by cost. Items 1–3 are log reads, no code change.

1. **Node log, the signature line.** `"Client not yet interested in packets (state=0)"`
   (`src/mesh/PhoneAPI.cpp:1884`). State `0` is `STATE_SEND_NOTHING` and proves the
   session is closed. Expect it alongside `"ToPhone queue is full, …"`
   (`src/mesh/MeshService.cpp:493`, `:498`).
2. **Node log, the close event.** `"Lost phone connection"`
   (`src/mesh/PhoneAPI.cpp:425`) followed by `"PhoneAPI::close()"` (`:359`). Its
   timestamp vs. the last `"Got client heartbeat"` (`:518`) tells you whether §3.5's
   keepalive gap is what opened the door.
3. **Proxy RTT log.** `"=== Phase 0 config-burst measurement ==="` present ⇒ a session
   completed. `"Bad frame length … resyncing"` (`src/uart_meshtastic.c:164`) or
   `"UART RX stopped"` (`:266`) ⇒ D3 is active. `"keepalive tx failed"`
   (`src/upstream_session.c:270`) ⇒ D3/§3.5 keepalive loss.
4. **Boot-window duration (the number this document does not have).** On the node,
   compare the log timestamp of the first line after `consoleInit()` against
   `NodeDB`'s first log line. Cheapest instrumented version: one
   `LOG_INFO("boot: consoleInit done millis=%u", millis())` after `src/main.cpp:421`
   and one `LOG_INFO("boot: nodeDB ready millis=%u", millis())` after `:854`. The delta
   is the window in which any `want_config` is silently discarded (D4).

## 7. Recommended fixes

**Proxy (owns the robustness; smallest change with the largest effect):**

1. **Retry `want_config` with backoff while state != `LIVE`.** Single highest-value fix:
   it closes §3.3, D4 and the D3-induced stall at once. Suggested shape: a
   `k_work_delayable` re-arming at e.g. 500 ms → 1 s → 2 s → capped at 5 s, cancelled on
   `CACHE_READY`.
2. **Add an upstream liveness watchdog** (already on the proxy's own v1.1 deferred list):
   if no *non-heartbeat* FromRadio has arrived in N minutes while phones are connected,
   assume the session died and re-issue `want_config`. Do **not** treat the keepalive's
   `queueStatus` reply as proof of a live session — §3.4 shows it is answered from a dead
   one.
3. **Check `ring_buf_put`'s return; add an inter-frame timeout to `rx_sm`; enlarge the
   ring and drain in bulk** (D3).
4. **Handle `FromRadio{rebooted}`** by re-running the upstream fetch (D2) — worth doing
   even though D1 currently prevents it from arriving at boot.

**Node / upstream Meshtastic (each independently reportable):**

5. `emitRebooted()` is unreachable because of the `canWrite` gate set in the same
   constructor (D1).
6. `close()` does not reset `canWrite` / `usingProtobufs` (D5).
7. A heartbeat is answered from a closed session, giving clients false liveness, and
   refreshes `lastContactMsec` without reviving the session (§3.4). Arguably a heartbeat
   on a closed serial session should either be ignored or trigger a `rebooted`-style
   "you need to re-request config" reply.
8. Early `ToRadio` is dropped with no diagnostic during the boot window (D4).
9. `MeshService::sendToPhone()` has no notion of whether a client is draining; the
   `MeshService.h:41-44` FIXME is still open.

## 8. Test-coverage gap

There is **no native test coverage for this path**. `test/test_serial/` contains only
`SerialModule.cpp` — the user-facing *SerialModule*, not `SerialConsole`/`StreamAPI`.
Nothing exercises the PhoneAPI state machine against a stream transport.

A `test_serial_console` suite driving `StreamAPI` with a fake `Stream` could cover, on
the host, with no hardware: session close on timeout → queue no longer drains; heartbeat
answered while closed; `want_config` revives and drains; short/failed writes; and the
`toPhoneQueue` overflow policy per portnum.
