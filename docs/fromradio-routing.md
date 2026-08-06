# FromRadio routing policy — per-variant disposition

**Status:** implemented. Source of truth: `src/router.c` (`router_dispatch`) and
`src/upstream_session.c` (`upstream_on_fromradio`). Tag numbers from
`build/meshtastic/mesh.pb.h` (NCS build).
**Related:** [`architecture.md`](./architecture.md),
[`upstream-session-robustness.md`](./upstream-session-robustness.md).

---

## Dispositions

- **broadcast** — sent to every connected phone.
- **unicast (1-1)** — sent to exactly one phone (by DST_ID).
- **absorb** — consumed by the proxy, never sent to any phone.
- **cache** — stored in the config cache during the boot fetch, later replayed
  1-1 per phone (a deferred form of absorb-then-replay).
- **swallow** — consumed and discarded (recovery re-fetch; the cache is not touched).

Disposition depends on the upstream session state. `FETCHING` = boot config
fetch; `REFETCHING` = recovery re-fetch; `LIVE` = normal routing.

## FromRadio variants (node → proxy)

| Variant | Tag | LIVE | FETCHING (boot) | REFETCHING (recovery) |
|---|---|---|---|---|
| `packet` | 2 | see packet sub-table | broadcast (falls through) | broadcast (falls through) |
| `my_info` | 3 | broadcast | cache | swallow |
| `node_info` | 4 | broadcast | cache | swallow |
| `config` | 5 | broadcast | cache | swallow |
| `log_record` | 6 | broadcast | cache | swallow |
| `config_complete_id` | 7 | broadcast | **terminator** → mark ready, LIVE (not cached) | **terminator** → LIVE |
| `rebooted` | 8 | **absorb → `upstream_refetch()`** | cache¹ | swallow¹ |
| `moduleConfig` | 9 | broadcast | cache | swallow |
| `channel` | 10 | broadcast | cache | swallow |
| `queueStatus` | 11 | **absorb** if it is our keepalive reply²; else broadcast | cache | swallow |
| `xmodemPacket` | 12 | broadcast | cache | swallow |
| `metadata` | 13 | broadcast | cache | swallow |
| `mqttClientProxyMessage` | 14 | broadcast | cache | swallow |
| `fileInfo` | 15 | broadcast | cache | swallow |
| `clientNotification` | 16 | broadcast | cache | swallow |
| `deviceuiConfig` | 17 | broadcast | cache | swallow |
| `lockdown_status` | 18 | broadcast | cache | swallow |

`id` (tag 1) is a scalar header field on the FromRadio message, not a payload
variant, so it never appears here.

**Note on FETCHING/REFETCHING:** the code does not enumerate variants there — any
non-`packet`, non-`config_complete_id` frame is cached (FETCHING) or swallowed
(REFETCHING) uniformly. The "cache/swallow" column lists what actually shows up in
a real boot burst; a stray variant would follow the same rule.

¹ **`rebooted` during a fetch** is an edge case (node and proxy booting together): it
would be cached/swallowed like any other burst frame rather than triggering a
refetch. Harmless — a fetch is already in progress (`upstream_refetch()` is a no-op
then anyway), and the fetch-retry watchdog covers a mid-fetch node reboot.

² The keepalive `queueStatus` is the reply the node sends to the proxy's own
heartbeat. It is answered **even from a dead session**, so it must never be
broadcast nor used as a liveness signal — see `upstream-session-robustness.md` §3.4.
Isolated via `upstream_swallow_live_queuestatus()`.

### `packet` (tag 2) sub-table — by decode + portnum

| Condition | Disposition |
|---|---|
| encrypted (not decoded) | broadcast (cannot inspect) |
| decoded, `portnum == PROXY_PORTNUM` (256), DST_ID registered | **unicast (1-1)** to that phone |
| decoded, `portnum == PROXY_PORTNUM`, DST_ID unregistered / bad header | broadcast (fallback) |
| decoded, any other portnum | broadcast |

## Proxy-generated FromRadio (synthesized, not from the node)

These never originate at the node; the proxy builds them and sends them **1-1**:

| Frame | When | Target |
|---|---|---|
| cached config burst (replay) | phone sends `want_config` and cache is ready | the requesting phone |
| `config_complete_id` (synthetic, phone's own nonce) | closes a phone's replayed burst | the requesting phone |
| `queueStatus` (synthetic, benign) | reply to a phone's `heartbeat` | the requesting phone |

## Appendix — ToRadio variants (phone → node), for symmetry

What the proxy does with each ToRadio it receives from a phone:

| Variant | Tag | Disposition |
|---|---|---|
| `packet` | 1 | forward to node over UART (PROXY_PORTNUM also route-traced) |
| `want_config_id` | 3 | **absorb** → serve from cache (replay), or park PENDING until ready |
| `disconnect` | 4 | **absorb** — never forwarded (would `close()` the shared node session). Per-phone slot teardown is a separate open item — see [`disconnect-flow.md`](./disconnect-flow.md) |
| `xmodemPacket` | 5 | forward (passthrough) |
| `mqttClientProxyMessage` | 6 | forward (passthrough) |
| `heartbeat` | 7 | **absorb** → synthesize a `queueStatus` reply for phone liveness |

The three absorbed ToRadio variants (`want_config_id`, `disconnect`, `heartbeat`)
are the proxy's virtualization boundary: they must never reach the node's single
shared PhoneAPI session.
