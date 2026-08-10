# Meshtastic PortNums — reference & proxy relevance

Values from the flashed firmware's `portnums.pb.h` (Meshtastic `PortNum` enum).
`portnum` lives in `MeshPacket.decoded.portnum` and is only readable on
**decoded** (plaintext) packets. See [`fromradio-routing.md`](./fromradio-routing.md)
for how the router dispatches by portnum.

## What matters for this proxy

- **256 `PRIVATE_APP` = `PROXY_PORTNUM`** (`src/proxy_protocol.h:33`). The carrier
  for the per-phone routing protocol: a decoded packet with portnum 256 is
  **unicast 1-1** by DST_ID from the embedded header. Everything else is broadcast.
- **Node-side rate limits (client → node).** The node's `PhoneAPI::handleToRadioPacket`
  throttles a few portnums; exceeding the window drops the packet and replies with a
  `queueStatus` (the `Rate limit portnum N` log). **256 is NOT throttled**, so
  proxy-framed traffic is exempt — only standard app traffic through the proxy is
  subject to these:

  | Portnum | # | Rate limit |
  |---|---|---|
  | `TEXT_MESSAGE_APP` | 1 | 1 per 2 s |
  | `POSITION_APP` | 3 | 1 per 10 s |
  | `WAYPOINT_APP` | 8 | 1 per 10 s |
  | `ALERT_APP` | 11 | 1 per 10 s |
  | `TELEMETRY_APP` | 67 | 1 per 10 s |
  | `TRACEROUTE_APP` | 70 | 1 per 30 s (broadcast multi-hop rejected) |

- **`TELEMETRY_APP` (67)** is the portnum you see most in the RX logs
  (`packet ... portnum=67`) — periodic device/environment telemetry from the mesh.
  Its cadence is also what sets the lower bound on the liveness-watchdog timeout N
  (see `upstream-session-robustness.md` §4).

## Full enum (flashed firmware)

| PortNum | # | Notes |
|---|---|---|
| `UNKNOWN_APP` | 0 | |
| `TEXT_MESSAGE_APP` | 1 | plain text; rate-limited node-side |
| `REMOTE_HARDWARE_APP` | 2 | |
| `POSITION_APP` | 3 | rate-limited |
| `NODEINFO_APP` | 4 | node identity; feeds the phone node list |
| `ROUTING_APP` | 5 | acks / routing errors |
| `ADMIN_APP` | 6 | device admin |
| `TEXT_MESSAGE_COMPRESSED_APP` | 7 | |
| `WAYPOINT_APP` | 8 | rate-limited |
| `AUDIO_APP` | 9 | |
| `DETECTION_SENSOR_APP` | 10 | |
| `ALERT_APP` | 11 | rate-limited |
| `KEY_VERIFICATION_APP` | 12 | |
| `REMOTE_SHELL_APP` | 13 | |
| `REPLY_APP` | 32 | |
| `IP_TUNNEL_APP` | 33 | |
| `PAXCOUNTER_APP` | 34 | |
| `STORE_FORWARD_PLUSPLUS_APP` | 35 | |
| `NODE_STATUS_APP` | 36 | |
| `SERIAL_APP` | 64 | |
| `STORE_FORWARD_APP` | 65 | |
| `RANGE_TEST_APP` | 66 | |
| `TELEMETRY_APP` | 67 | most-seen; dominates the RX log |
| `ZPS_APP` | 68 | |
| `SIMULATOR_APP` | 69 | |
| `TRACEROUTE_APP` | 70 | rate-limited |
| `NEIGHBORINFO_APP` | 71 | |
| `ATAK_PLUGIN` | 72 | |
| `MAP_REPORT_APP` | 73 | |
| `POWERSTRESS_APP` | 74 | |
| `LORAWAN_BRIDGE` | 75 | |
| `RETICULUM_TUNNEL_APP` | 76 | |
| `CAYENNE_APP` | 77 | |
| `GROUPALARM_APP` | 112 | |
| **`PRIVATE_APP`** | **256** | **`PROXY_PORTNUM` — the proxy's 1-1 routing carrier** |
| `ATAK_FORWARDER` | 257 | |
| `MAX` | 511 | upper bound of the range |

> Enum values can shift between firmware releases. Re-derive from the node's
> `portnums.pb.h` if you upgrade; the only value the proxy hard-codes is
> `PROXY_PORTNUM = 256` (`src/proxy_protocol.h`).
