# Client Integration Guide — Android / iOS apps ↔ Meshtastic BLE Proxy

> Audience: developers of the phone apps that connect to the nRF52840 proxy.
> Companion to [`architecture.md`](./architecture.md).

The proxy presents itself as a **Meshtastic-compatible BLE peripheral** that
multiplexes up to 6 phones onto **one** physical Meshtastic node over UART. It
supports **two delivery models that run in parallel**:

1. **Broadcast** — works with the **stock Meshtastic app, no changes**. Every
   phone behaves like a client of the same node.
2. **Router** — per-phone addressing via a custom proxy protocol. Requires
   **app-side changes** (this guide).

---

## 1. GATT service & characteristics

Service UUID `6ba1b218-15a8-461f-9fa8-5dcae273eafd` (same as stock Meshtastic).

| Characteristic | UUID | Props | Notes |
|---|---|---|---|
| **FROMNUM** | `ed9da18c-a800-4f66-a670-aa7547e34453` | read, **notify** | LE uint32 packet counter; subscribe to know when to drain FROMRADIO |
| **FROMRADIO** | `2c55e69e-4993-11ed-b878-0242ac120002` | **read** | drain repeatedly until a 0-length read (one `FromRadio` per read) |
| **TORADIO** | `f75c76d2-129e-4dad-a1dd-7866124401e7` | **write** / write-no-rsp | write one `ToRadio` |
| **LOGRADIO** | `5a3d6e49-06e6-4423-9944-e9de8cdf9547` | read, notify | **stub** in v1.0 (returns empty) |
| **NODE_REG** *(custom)* | `12345678-0001-0001-0001-000000000001` | **write** (4 bytes) | router only — register this phone's `proxy_id`. Ignored by the stock app. |

Note: there is **no BLE Battery Service** (`0x180F`/`0x2A19`). Battery/metrics
arrive via protobuf (`NodeInfo.device_metrics` + `Telemetry` portnum 67), not BLE.

---

## 2. Connection lifecycle (both models)

The proxy reproduces the real node's handshake per phone. The app does exactly
what it does against a real node:

1. Discover service + characteristics.
2. Subscribe (CCCD) to **FROMNUM** notifications.
3. **Phase 1 config:** write `ToRadio{ want_config_id = 69420 }` (ONLY_CONFIG) →
   drain FROMRADIO until `FromRadio{ config_complete_id == 69420 }`.
4. **Phase 2 nodes:** write `ToRadio{ want_config_id = 69421 }` (ONLY_NODES) →
   drain until `config_complete_id == 69421`.
   *(A normal/random nonce returns the full superset in one round; the proxy
   echoes whatever nonce you send and serves the matching segment.)*
5. **Heartbeat:** send `ToRadio{ heartbeat = { nonce } }` periodically
   (Android ~30 s). **nonce must never be 1** (special on the node). The proxy
   replies a `queueStatus` to keep your liveness timer alive.

The proxy serves config from a **boot-time cache**: node metrics (battery,
utilization, last_heard, position) may be slightly stale at connect, then refresh
from live telemetry/position packets. See `architecture.md` for the full lifecycle.

---

## 3. Broadcast model — NO app changes

Standard Meshtastic portnums (TEXT=1, POSITION=3, ROUTING=5, ADMIN=6,
TELEMETRY=67, …) are **broadcast to every connected phone**. Filter by
`id`/`requestId`/channel as usual.

**Caveats of the shared-node model** (design into the UX):
- **Shared identity:** all phones share the node's `nodenum`. To the mesh, every
  phone's traffic appears to come from the same node; DMs/ACKs to that node reach
  **all** phones. Phones are not distinguishable endpoints at the mesh layer.
- **Shared config:** admin/config commands change the **node** (affect all
  phones). Consider restricting who may send admin.
- **Privacy:** every phone sees all of the node's traffic on shared channels.

If per-phone identity/addressing matters, use the router model below.

---

## 4. Router model — per-phone addressing (app changes required)

Gives each phone its own address (`proxy_id`) on top of the shared node. Uses
Meshtastic **portnum 256** (`PRIVATE_APP`) as the carrier and a small header the
proxy reads to route to the right BLE connection.

### 4.1 Wire format (the proxy header)

Carried inside `MeshPacket.decoded.payload` when `portnum == 256`:

```
offset  field      size
  0     VERSION    1     = 0x01
  1     SRC_ID     4     sender proxy_id
  5     DST_ID     4     receiver proxy_id
  9     content    N     payload, N ≤ 141 bytes
```
Header = 9 bytes. Practical content max = **141 bytes** (Meshtastic payload
limit). `proxy_id` = **4 bytes** = a little-endian `uint32`. (Defined in
`src/proxy_protocol.h` as `PROXY_ID_SIZE`.)

> The current build carries the phone identity as a 4-byte `uint32` (e.g. a
> national phone number without country code — the proxy logs it as
> `+56<uint32>`). The 16-byte UUID form is a possible future format: bump
> `PROXY_ID_SIZE` to 16 and the header/offsets/content-max recompute from it, and
> the UUID string renderers in `src/proxy_protocol.c` are kept for that case.
> Both ends must agree on `PROXY_ID_SIZE` — it defines the wire layout.

> **Proposed v2:** a 2-byte monotonic `seq` per `(SRC_ID, DST_ID)` pair inserted
> at offset 9, before the content, so a receiver can compute PDR and loss-burst
> statistics from its own log alone. Not implemented — see
> [`seq-numbers-pdr.md`](./seq-numbers-pdr.md).

### 4.2 The four app changes

**(1) Register on connect — write NODE_REG.**
After connecting, write your 4-byte `proxy_id` (LE `uint32`) to the NODE_REG
characteristic. A write of any other length is rejected
(`BT_ATT_ERR_INVALID_ATTRIBUTE_LEN`). Without it the proxy cannot map
`DST_ID → connection` and falls back to broadcast. (The stock app simply never
writes it → broadcast.)

**(2) Send — frame + portnum 256.**
Build the 9-byte header + content, then send a normal Meshtastic
`ToRadio{ MeshPacket{ to=<dest nodenum>, decoded.portnum=256, payload=<header+content> } }`.
The app does the framing; the proxy forwards it verbatim to UART → node → mesh.

**(3) Receive — parse portnum 256.**
On a `FromRadio` packet with `portnum == 256`, parse the header, confirm
`DST_ID == your proxy_id`, and extract `SRC_ID` + content. (The proxy already
routed it to you; broadcast fallback may also deliver it.)

**(4) Two-level addressing (key design point).**
There are two distinct addresses:
- `MeshPacket.to` = **nodenum** — which Meshtastic node hosts the target phone.
- `DST_ID` (proxy_id) = **which phone** behind that node.

The app/system must maintain a **`proxy_id → nodenum` directory** to know which
node to address. That directory (a discovery service or provisioning step) is new
app-side logic.

### 4.3 Fallback / coexistence
- If a `DST_ID` is not registered (target phone not connected, or stock app),
  the proxy **broadcasts** the packet — no silent drop.
- Router and broadcast coexist: use standard portnums for normal Meshtastic
  features (broadcast) and portnum 256 for targeted per-phone messaging.
- Targeted routing requires the packet to arrive **decoded** at the proxy — i.e.
  on a channel whose key the proxy's node holds; otherwise it can't read the
  header and broadcasts.

---

## 5. Constraints / gotchas
- `proxy_id`: 4 bytes (LE `uint32`); content ≤ 141 bytes per message.
- Heartbeat nonce ≠ 1; want_config special nonces 69420 / 69421.
- FROMRADIO is **read-only** (do NOT expect notifications on it — only FROMNUM
  notifies). Drain on FROMNUM notify and after each write.
- Never expect a non-zero read forever: drain until a 0-length read.
- LOGRADIO is a stub in v1.0.

## 6. References (this repo)
- Architecture + diagrams: [`docs/architecture.md`](./architecture.md)
- Proxy header format & constants: `src/proxy_protocol.h`
- GATT server + routing: `src/ble_gatt.c`, `src/router.c`
