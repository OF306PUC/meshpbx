/*
 * router.c — FromRadio dispatch: UART bytes → correct BLE connection(s)
 *
 * Two-tier routing:
 *
 * Tier 1 — Meshtastic variant level (which_payload_variant):
 *   Non-packet variants (NodeInfo, MyInfo, Channel, Config, ConfigComplete…)
 *   are always broadcast.  Every phone needs the full config handshake.
 *
 * Tier 2 — Application level (portnum + proxy header):
 *
 *   portnum == PROXY_PORTNUM (256):
 *     Parse the proxy header from Data.payload.bytes.
 *     Route the FULL raw FromRadio bytes to the BLE connection whose
 *     registered proxy_id matches DST_ID in the header.
 *     Fallback to broadcast if:
 *       - Payload too short to contain a valid header
 *       - No connection has registered the target DST_ID yet (TODO: no broadcast)
 *
 *   portnum != PROXY_PORTNUM (standard Meshtastic traffic):
 *     Broadcast to all connections.
 *     The official Meshtastic app handles standard portnums (TEXT, POSITION,
 *     TELEMETRY, etc.) and the phone's app filters what it cares about.
 *
 *   Packet not decoded (encrypted):
 *     Broadcast raw bytes (cannot inspect portnum).
 */

#include "router.h"
#include "ble_gatt.h"
#include "proxy_protocol.h"
#include "upstream_session.h"
#include "route_trace.h"

#include "meshtastic/mesh.pb.h"   /* meshtastic_FromRadio_packet_tag */
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <inttypes.h>

LOG_MODULE_REGISTER(router, LOG_LEVEL_DBG);

void router_dispatch(const uint8_t *raw_bytes, uint16_t len,
                     const struct fromradio_info *info)
{
    /* ----------------------------------------------------------------
     * Upstream config-fetch window: while the proxy is consuming its
     * own boot config burst, config/meta variants are CONSUMED into the cache
     * (not broadcast). upstream_on_fromradio returns true when it took the frame.
     * Packet variants are NOT consumed (returns false) → fall through so live
     * mesh traffic still reaches the phones during the fetch.
     * ---------------------------------------------------------------- */
    enum upstream_state ust = upstream_get_state();
    if (ust == UPSTREAM_FETCHING || ust == UPSTREAM_REFETCHING) {
        if (upstream_on_fromradio(raw_bytes, len, info)) {
            return;
        }
        /* else: packet variant — fall through to the normal broadcast path. */
    }

    /* ------------------------ Broadcast Path ------------------------ */
    
    /* ----------------------------------------------------------------
     * Tier 1: non-packet FromRadio variant → broadcast unconditionally,
     * EXCEPT the queueStatus the node sends in reply to our own upstream
     * keepalive heartbeat - that reply is for the proxy, not the
     * phones, so swallow it instead of broadcasting noise.
     * ---------------------------------------------------------------- */
    if (info->which_variant != meshtastic_FromRadio_packet_tag) {
        if (info->which_variant == meshtastic_FromRadio_queueStatus_tag &&
            upstream_swallow_live_queuestatus()) {
            LOG_DBG("keepalive queueStatus swallowed (not broadcast)");
            return;
        }
        ble_gatt_broadcast_fromradio(raw_bytes, len);
        return;
    }

    /* ----------------------------------------------------------------
     * Packet variant — check if it's decoded (plaintext) or encrypted.
     * Encrypted packets cannot be inspected; broadcast as fallback.
     * ---------------------------------------------------------------- */
    if (!info->is_decoded) {
        ble_gatt_broadcast_fromradio(raw_bytes, len);
        return;
    }

    /* ----------------------------------------------------------------
     * Tier 2: decoded packet — route by portnum
     * ---------------------------------------------------------------- */
    if (info->portnum == PROXY_PORTNUM) {
        /* Proxy protocol: route by DST_ID embedded in the payload header. */
        struct proxy_header hdr;

        if (!proxy_header_parse(info->payload_bytes, info->payload_len, &hdr)) {
            LOG_WRN("PROXY_PORTNUM: bad header (len=%u) — broadcast fallback",
                    info->payload_len);
            ble_gatt_broadcast_fromradio(raw_bytes, len);
            return;
        }
#if IS_ENABLED(CONFIG_MESHTASTIC_ROUTE_TRACE)
        /* TESTING ONLY: Assumes proxy-framed payload with 4-byte IDs. */
        ROUTE_TRACE("ROUTE DN  hdr=[%02x][0x%08" PRIx32 "][0x%08" PRIx32 "] content='%.*s'",
                    info->payload_bytes[PROXY_OFF_VERSION],
                    sys_get_be32(&info->payload_bytes[PROXY_OFF_SRC]),
                    sys_get_be32(&info->payload_bytes[PROXY_OFF_DST]),
                    (int)(info->payload_len - PROXY_HEADER_SIZE),
                    (const char *)&info->payload_bytes[PROXY_OFF_CONTENT]);
        ROUTE_TRACE_HEXDUMP(info->payload_bytes, info->payload_len, "ROUTE DN payload");
#endif
        struct bt_conn *conn = ble_gatt_get_conn_by_proxy_id(&hdr.dst);
        if (conn != NULL) {
            ble_gatt_enqueue_fromradio(conn, raw_bytes, len);
        } else {
            /*
             * No phone has registered this DST_ID yet (or it's not connected).
             * Broadcast so the message isn't silently dropped. Once the target
             * phone connects and calls NODE_REG, subsequent messages are targeted.
             */
            LOG_DBG("proxy: dst=+56%" PRIu32 " not registered -> broadcast fallback",
                    sys_get_be32(hdr.dst.bytes));
            ble_gatt_broadcast_fromradio(raw_bytes, len);
        }
        return;
    }

    /* Standard Meshtastic portnum (TEXT_MESSAGE, POSITION, TELEMETRY, etc.)
     * → broadcast to all connected phones. The app filters what it needs. */
    ble_gatt_broadcast_fromradio(raw_bytes, len);
}
