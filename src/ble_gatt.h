#ifndef BLE_GATT_H
#define BLE_GATT_H

#include <stdint.h>
#include <zephyr/bluetooth/conn.h>
#include "proxy_protocol.h"

/* Max simultaneous BLE connections (phones).
 * Tied to CONFIG_BT_MAX_CONN so the app-layer slot array always matches the
 * BT stack limit. Change prj.conf (CONFIG_BT_MAX_CONN) to resize both at once. */
#define MAX_BLE_CONNECTIONS     CONFIG_BT_MAX_CONN

/* Per-connection FromRadio packet queue depth */
#define FROMRADIO_QUEUE_DEPTH   16

/* Max size of a single FromRadio protobuf packet (bytes) */
#define FROMRADIO_MAX_PKT_SIZE  512

/*
 * Per-phone config-session state.
 *
 *   CONNECTED          → just connected (CCCD not yet subscribed)
 *   AWAIT_WANT_CONFIG  → subscribed, waiting for the phone's want_config
 *   PENDING            → asked for config before the cache was ready; queued in
 *                        upstream_session and served once CACHE_READY
 *   REPLAYING          → mid burst replay (cursor walking the shared cache)
 *   ACTIVE             → burst replayed + config_complete sent; live traffic
 */
enum phone_state {
    PHONE_CONNECTED,
    PHONE_AWAIT_WANT_CONFIG,
    PHONE_PENDING,
    PHONE_REPLAYING,
    PHONE_ACTIVE,
};

/**
 * Callback fired when a connected phone writes to the TORADIO characteristic.
 *
 * @param conn  BLE connection that sent the data.
 * @param data  Raw protobuf bytes (ToRadio message, already encoded by the app).
 * @param len   Length in bytes.
 */
typedef void (*toradio_cb_t)(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/** Initialize GATT service and connection tracking. Must be called before bt_enable(). */
int ble_gatt_init(toradio_cb_t cb);

/** Start BLE advertising (Meshtastic-compatible). Called at boot and after each connection. */
int ble_gatt_start_advertising(void);

/**
 * Enqueue a FromRadio packet for a specific BLE connection.
 * Triggers a FROMNUM notification to the phone.
 *
 * @return  0        success
 *         -ENOENT   connection not tracked
 *         -ENOMEM   per-connection queue full
 *         -EMSGSIZE packet exceeds FROMRADIO_MAX_PKT_SIZE
 */
int ble_gatt_enqueue_fromradio(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/**
 * Enqueue a FromRadio packet for ALL currently connected BLE clients.
 * Used for config/meta variants and broadcast mesh packets.
 *
 * @return Number of connections that received the packet (0 if none connected).
 */
int ble_gatt_broadcast_fromradio(const uint8_t *data, uint16_t len);

/**
 * Return the BLE connection whose registered proxy_id matches the given id.
 * Returns NULL if no connection has registered that id.
 *
 * Used by the router to target a specific phone by its proxy identifier.
 */
struct bt_conn *ble_gatt_get_conn_by_proxy_id(const proxy_id_t *id);

/**
 * Register a proxy identifier (phone number, UUID, etc.) for a BLE connection.
 *
 * Called from the NODE_REG GATT write handler when a phone writes its 16-byte
 * identifier.  The router uses this mapping to perform targeted delivery.
 *
 * @param conn  The BLE connection registering.
 * @param id    16-byte proxy identifier written by the phone.
 * @return 0 on success, -ENOENT if connection is not tracked.
 */
int ble_gatt_register_proxy_id(struct bt_conn *conn, const proxy_id_t *id);

/*
 * Replay the boot-populated config cache to a single phone, then close the
 * burst with a synthesized FromRadio{config_complete_id = nonce} carrying THAT
 * phone's nonce (per-phone replay).
 *
 * Frames are read straight from the shared config_cache arena via a cursor and
 * enqueued one by one — the whole burst is never copied into the per-conn queue
 * as a block (avoids N× RAM duplication). On completion the phone goes ACTIVE.
 *
 * Called from on_toradio_ble (cache already ready) and registered with
 * upstream_session as the serve callback for PENDING phones.
 */
void ble_gatt_replay_cached_burst(struct bt_conn *conn, uint32_t nonce);

/*
 * Park a phone as PENDING: it asked for config before the cache was ready.
 * Records the phone's nonce and registers it with upstream_session, which
 * replays the burst (via the serve callback) once CACHE_READY.
 */
void ble_gatt_park_pending(struct bt_conn *conn, uint32_t nonce);

/*
 * Reactive BLE liveness reply. Enqueue a synthesized FromRadio{
 * queueStatus} to this connection (bumps fromnum + notifies FROMNUM) in
 * response to the phone's ToRadio.heartbeat. The phone reads it as "radio
 * data", keeping its app-layer liveness timer from tripping. Absorbed locally
 * — the heartbeat is NEVER forwarded to UART. The queueStatus is encoded once
 * and reused.
 */
void ble_gatt_reply_queuestatus(struct bt_conn *conn);

/*
 * Release this phone's slot after it sent ToRadio{disconnect}.
 *
 * Marks the slot as leaving (further FromRadio frames are refused with
 * -ESHUTDOWN) and schedules the BLE link teardown ~250 ms later, which triggers
 * the normal on_disconnected() → free_slot() cleanup.
 *
 * Why the proxy forces this rather than waiting for the phone: the app intends
 * to drop the link itself, but cannot guarantee it. FlutterBluePlus queues its
 * disconnect behind other operations for the device, skips the wait when the
 * platform reports no state change, and is subject to the documented Android
 * "stranded connection" race (no gatt handle → the link can never be closed
 * from the app side). Link supervision does not rescue this either: the
 * observed leak survived 3+ minutes against a 4 s configured timeout, because
 * the connection stayed alive at the link layer while being dead at the app
 * layer. The ToRadio{disconnect} write is the one signal that reliably arrives,
 * so it is treated as authoritative.
 *
 * Safe when the app's own disconnect wins the race — bt_conn_disconnect() then
 * returns -ENOTCONN and the teardown is a no-op.
 *
 * @return  0        teardown scheduled
 *         -ENOENT   connection not tracked
 */
int ble_gatt_request_teardown(struct bt_conn *conn);


#endif /* BLE_GATT_H */
