# Disconnect flow — proxy ↔ phone app contract (open question)

**Status:** open question for the phone-app developer. The proxy behaviour below
is implemented; the per-phone slot teardown is **pending** the app-side answer in §5.
**Audience:** phone-app developer + proxy firmware. The goal is one shared, explicit
picture of what happens on each side when a phone ends its session.
**Related:** [`upstream-session-robustness.md`](./upstream-session-robustness.md),
[`client-integration.md`](./client-integration.md), [`tophone-queue-stall.md`](./tophone-queue-stall.md).

---

## 1. The three actors

```
   phone app  ──BLE──▶  proxy (nRF52840)  ──UART──▶  node (Meshtastic)
```

The proxy multiplexes up to `MAX_BLE_CONNECTIONS` phones onto the node's **single**
serial PhoneAPI session. Each phone gets a per-phone slot in the proxy
(`struct proxy_conn`), with its own FromRadio queue (`FROMRADIO_QUEUE_DEPTH = 8`)
that the phone drains by reading the FROMRADIO characteristic.

## 2. What `ToRadio{disconnect}` means on each side

- **On the node (stock Meshtastic):** `PhoneAPI::handleToRadio` on the
  `disconnect` variant calls `close()` → `STATE_SEND_NOTHING`. In that state the
  node stops draining its `toPhoneQueue` and **only** a fresh `want_config` can
  revive it. Because the node session is *shared* by all phones, one phone's
  disconnect must never reach the node.

- **On the proxy (current behaviour):** `on_toradio_ble` **absorbs**
  `ToRadio{disconnect}` and does **not** forward it over UART (mirrors how
  `want_config` and `heartbeat` are absorbed). This protects the shared node
  session — confirmed root cause of the original `toPhoneQueue` stall, see
  `tophone-queue-stall.md`.

So far so good: the node is shielded. The open question is what the proxy should
do with **its own per-phone slot** for the departing phone.

## 3. Observed hardware behaviour (two variants of the same event)

From a real capture (`logs_nordic.txt`). Two phones, same `ToRadio{disconnect}`,
two different endings:

**Variant A — disconnect, then BLE link kept up (slot leaks):**
```
00:31:35  main: ToRadio disconnect from conn 0x20004380 — absorbed (not forwarded)
          (no BLE "Disconnected" event follows)
00:34:25  ble_gatt: FromRadio queue full for conn 0x20004380
          ble_gatt: broadcast: slot 0 enqueue failed: -12   (-ENOMEM)
          ... repeats indefinitely ...
```
The app closed its API session but left the BLE link up **and stopped reading
FROMRADIO**. The slot stays allocated, the router keeps enqueuing FromRadio to it,
and the 8-deep queue overflows on every frame. The slot is effectively **leaked**.

**Variant B — disconnect, then BLE link dropped (clean):**
```
00:31:42  main: ToRadio disconnect from conn 0x20004440 — absorbed (not forwarded)
00:31:43  ble_gatt: Disconnected [slot 0] (reason 0x13)   (remote user terminated)
```
The app sent disconnect **and** tore down the BLE link. The proxy's
`on_disconnected` callback ran `free_slot()` — the slot was released cleanly.

**Important:** in both cases the **node session stayed alive and healthy**
(FromRadio kept flowing, FROMNUM advancing). The queue that overflowed in Variant A
is the **proxy's per-phone queue**, not the node's `toPhoneQueue`. Impact of the
leak is local to that slot: wasted slot capacity + endless warnings; other phones
and the node are unaffected.

## 4. Why the proxy cannot clean up on its own today

The proxy frees a slot only in `on_disconnected` (the BLE-stack disconnect
callback → `free_slot()`). It has no other teardown trigger. So a phone that ends
its API session with `ToRadio{disconnect}` **without** dropping the BLE link leaves
its slot allocated until the BLE supervision timeout eventually drops the link — and
Variant A shows that can be many minutes (or never, if the OS keeps the link warm
for a backgrounded app).

## 5. The question for the app developer

**After your app sends `ToRadio{disconnect}`, what does it do with the BLE link?**

1. **Closes the BLE connection** (Variant B). → No proxy change needed; the existing
   `on_disconnected` path already cleans up. This is the cleanest contract.
2. **Keeps the BLE link up and idles** (Variant A, observed). → The proxy must tear
   the slot down itself on receiving `disconnect`. Proposed proxy behaviour:
   on absorbing `disconnect`, call `bt_conn_disconnect(conn, REMOTE_USER_TERM)`,
   which triggers the existing `on_disconnected` → `free_slot()` cleanup.
3. **Keeps the BLE link up and later re-sends `want_config` to resume** on the same
   link. → Force-disconnecting (option 2) would be wrong; the proxy would instead
   need to *quiesce* the slot (stop enqueuing, reset its queue/replay state) while
   keeping the link and registration, and re-activate on the next `want_config`.

**Proxy's default recommendation:** treat `disconnect` as "the phone is leaving"
and force the BLE teardown (option 2). It reuses the one existing cleanup path and
matches what a well-behaved client does anyway (Variant B). We will implement this
**unless** the app relies on the resume-on-same-link flow (option 3), in which case
we implement the quiesce variant instead.

## 6. Desired end-to-end flow (proposed, pending §5 answer)

```
phone app                proxy                         node
   │  ToRadio{disconnect} │                            │
   ├─────────────────────▶│  absorb (never forward) ───┼─▶ (node session untouched)
   │                      │  tear down / quiesce slot  │
   │  (close BLE link)    │                            │
   ├─────────────────────▶│  on_disconnected → free    │
```

The two proxy actions — **absorb toward the node** and **release the slot toward
the phone** — are independent and both required. Today only the first is done; §5
decides the shape of the second.
