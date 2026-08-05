#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <inttypes.h>
#include "ble_gatt.h"
#include "uart_meshtastic.h"
#include "proto_handler.h"
#include "router.h"
#include "upstream_session.h"
#include "proxy_protocol.h"
#include "route_trace.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * Called (system work-queue context) when the UART driver has assembled a
 * complete FromRadio frame from the Meshtastic node.
 *
 * Flow:
 *   1. Decode the protobuf (nanopb) to extract routing fields.
 *   2. Dispatch raw bytes to the correct BLE connection(s) via the router.
 *
 * On decode failure the raw bytes are still broadcast so no packet is dropped.
 */
static void on_fromradio_uart(const uint8_t *payload, uint16_t len)
{
    struct fromradio_info info;

    if (proto_decode_fromradio(payload, len, &info) != 0) {
        /* Decode failed (malformed frame?). Broadcast raw as fallback. */
        LOG_WRN("FromRadio decode failed — broadcasting raw (%d B)", len);
        ble_gatt_broadcast_fromradio(payload, len);
        return;
    }

    router_dispatch(payload, len, &info);
}

/*
 * Called when a phone writes a ToRadio protobuf to the GATT TORADIO
 * characteristic (per-phone config-session virtualization).
 *
 * The proxy is NO LONGER a blind passthrough:
 *   - want_config_id : served LOCALLY from the cache (replay or PENDING).
 *                      NEVER forwarded to UART — forwarding it would restart the
 *                      node's single global config session and clobber every
 *                      other phone's in-flight handshake.
 *   - heartbeat      : absorbed locally — answered with a synthesized
 *                      queueStatus for liveness; NEVER forwarded to UART. The
 *                      node's serial link is kept alive by the proxy's own
 *                      keepalive, and delivery of inbound FromRadio is driven by
 *                      the node's push (not by client ToRadio), so forwarding
 *                      heartbeats is unnecessary.
 *   - anything else  : a real packet → forwarded to the node via UART as today.
 *
 * On decode failure we forward the raw bytes best-effort (no silent drop).
 */
static void on_toradio_ble(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    struct toradio_info ti;

    if (proto_decode_toradio(data, len, &ti) != 0) {
        /* Malformed ToRadio — forward best-effort so nothing is silently lost. */
        LOG_WRN("ToRadio decode failed (%d B) from conn %p — forwarding raw",
                len, (void *)conn);
        (void)uart_meshtastic_tx(data, len);
        return;
    }

    if (ti.has_want_config) {
        /* Serve config locally: replay if the cache is ready, else PENDING. */
        enum upstream_state st = upstream_get_state();

        if(st != UPSTREAM_LIVE && st != UPSTREAM_CACHE_READY) {
            return;
        }

        if (st == UPSTREAM_CACHE_READY || st == UPSTREAM_LIVE || st == UPSTREAM_REFETCHING) {
            LOG_INF("want_config nonce=%u from conn %p → replay cached burst",
                    (unsigned)ti.want_config_id, (void *)conn);
            ble_gatt_replay_cached_burst(conn, ti.want_config_id);
        } else {
            LOG_INF("want_config nonce=%u from conn %p → PENDING (cache not ready)",
                    (unsigned)ti.want_config_id, (void *)conn);
            ble_gatt_park_pending(conn, ti.want_config_id);
        }
        return;  /* NEVER forward want_config to UART. */
    }

    if (ti.has_heartbeat) {
        /* Absorbed locally: reply a synthesized queueStatus to keep the
         * phone's liveness timer alive. */
        ble_gatt_reply_queuestatus(conn);
        return;
    }

    if (ti.has_disconnect) {
        /* Absorbed locally: never forward through UART to keep an active PhoneAPI session.
         * This keeps the drain of toPhoneQueue*/
        LOG_INF("ToRadio disconnect from conn %p — absorbed (not forwarded)", (void *)conn);
        return;
    }

#if IS_ENABLED(CONFIG_MESHTASTIC_ROUTE_TRACE)
    /* TESTING ONLY: Assumes proxy-framed payload with 4-byte IDs. */
    if (ti.is_packet && ti.portnum == PROXY_PORTNUM && ti.payload_len >= PROXY_HEADER_SIZE) {
        ROUTE_TRACE("ROUTE UP  hdr=[%02x][0x%08" PRIx32 "][0x%08" PRIx32 "] content='%.*s'",
                    ti.payload_bytes[PROXY_OFF_VERSION],
                    sys_get_be32(&ti.payload_bytes[PROXY_OFF_SRC]),
                    sys_get_be32(&ti.payload_bytes[PROXY_OFF_DST]),
                    (int)(ti.payload_len - PROXY_HEADER_SIZE),
                    (const char *)&ti.payload_bytes[PROXY_OFF_CONTENT]);
        ROUTE_TRACE_HEXDUMP(ti.payload_bytes, ti.payload_len, "ROUTE UP payload");
    }
#endif
    /* Real packet → forward to the node, and push the upstream keepalive out
     * the keepalive only fires after a stretch of true silence. */
    int err = uart_meshtastic_tx(data, len);
    if (err == -ENOMEM) {
        LOG_WRN("TX queue full — ToRadio from conn %p dropped", (void *)conn);
    } else if (err) {
        LOG_ERR("uart_meshtastic_tx: %d", err);
    } else {
        upstream_keepalive_reschedule();
    }
}

int main(void)
{
    int err;

    LOG_INF("=== Meshtastic BLE Proxy starting ===");

    /* 1. Init GATT service (must happen before bt_enable). */
    err = ble_gatt_init(on_toradio_ble);
    if (err) {
        LOG_ERR("ble_gatt_init: %d", err);
        return err;
    }

    /* 2. Enable Bluetooth stack. */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("bt_enable: %d", err);
        return err;
    }
    LOG_INF("Bluetooth ready");

    /* 3. Start advertising as a Meshtastic-compatible peripheral. */
    err = ble_gatt_start_advertising();
    if (err) {
        return err;
    }

    /* 4. Init UART driver — starts DMA reception immediately.
     *    The Meshtastic node begins sending FromRadio frames once it receives
     *    the proxy's own want_config_id ToRadio (sent by upstream_session_start
     *    below), not a phone's — phones are served from the cache. */
    err = uart_meshtastic_init(on_fromradio_uart);
    if (err) {
        LOG_ERR("uart_meshtastic_init: %d", err);
        return err;
    }

    /* 5. Wire the per-phone replay callback BEFORE starting the upstream fetch,
     *    so any PENDING phone queued during FETCHING is served the instant the
     *    cache becomes ready. */
    upstream_set_serve_cb(ble_gatt_replay_cached_burst);

    /* 6. Kick off the proxy's own boot want_config: fetch the node's config
     *    burst into the cache, then go LIVE. Phones connecting before the cache
     *    is ready are parked PENDING and replayed once ready. */
    upstream_session_start();

    /* Main loop — all work runs in the system work queue and BT callbacks. */
    while (1) {
        k_sleep(K_MSEC(100));
    }

    return 0;
}
