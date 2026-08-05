#ifndef UPSTREAM_SESSION_H
#define UPSTREAM_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>   /* struct bt_conn — BT stack, not ble_gatt */

#include "proto_handler.h"           /* struct fromradio_info */

/*
 * upstream_session — the proxy's single config session with the node.
 *
 * At boot the proxy issues its OWN want_config (its own nonce) to the node over
 * UART, consumes the ordered FromRadio burst into config_cache, and on the
 * matching config_complete_id marks the cache ready and goes LIVE. Phones that
 * connect before the cache is ready are queued PENDING and served once ready.
 *
 * Threading: all entry points run on the single Zephyr system work queue (the
 * UART RX callback path and the BLE callback path are both work-queue context).
 * No mutexes; state is a plain enum mutated only from that single context.
 *
 * NOTE: this module has NO dependency on ble_gatt. It only references
 * `struct bt_conn` (an opaque BT-stack type) and stubs the per-phone replay
 * (Task C wires the real replay into ble_gatt).
 */

enum upstream_state {
    UPSTREAM_BOOT,         /* nothing started yet                            */
    UPSTREAM_FETCHING,     /* want_config sent; consuming the burst → cache  */
    UPSTREAM_CACHE_READY,  /* burst complete, cache published (transient)    */
    UPSTREAM_LIVE,         /* normal routing; FromRadio no longer cached     */
    UPSTREAM_REFETCHING,   /* recovery re-fetch; consumes the burst but does NOT touch the cache */
};

/*
 * Kick off the boot fetch: pick a nonce, encode a ToRadio{want_config_id}, send
 * it over UART, call cache_begin(), and move to UPSTREAM_FETCHING.
 *
 * The nonce is sys_rand32_get(), forced to be neither 0 nor 1: nonce 1 is
 * special on the node — it forces a NodeInfo rebroadcast — so it must never be
 * used as a plain config nonce. If the RNG yields 0 or 1 it is bumped to 2.
 *
 * Must be called after uart_meshtastic_init() (so TX can be enqueued).
 */
void upstream_session_start(void);

/* Current upstream state. */
enum upstream_state upstream_get_state(void);

/*
 * Register the per-phone replay callback (dependency inversion).
 *
 * upstream_session must serve PENDING phones once the cache is ready, but it has
 * NO dependency on ble_gatt. main wires
 * ble_gatt_replay_cached_burst here at boot; serve_one_pending() invokes it.
 *
 * @param cb  called as cb(conn, nonce). upstream passes nonce = 0, meaning
 *            "use the phone's nonce stored by ble_gatt_park_pending" (upstream
 *            tracks only the conn, not the nonce).
 */
void upstream_set_serve_cb(void (*cb)(struct bt_conn *conn, uint32_t nonce));

/*
 * Feed one decoded FromRadio frame to the upstream state machine.
 *
 * @param payload  raw FromRadio protobuf bytes (no serial header).
 * @param len      payload length.
 * @param info     already-decoded routing info for this frame.
 *
 * @return true  if the frame was CONSUMED by the session (added to the cache or
 *               it was the closing config_complete_id) — the caller must NOT
 *               broadcast it.
 *         false if the caller still owns the frame (live traffic / packet
 *               variants, or anything received while LIVE) — the caller routes
 *               it as usual.
 */
bool upstream_on_fromradio(const uint8_t *payload, uint16_t len,
                           const struct fromradio_info *info);

/*
 * Register a phone that asked for config before the cache was ready. It is
 * remembered as PENDING and served (replayed) once the cache becomes ready.
 * Safe to call repeatedly with the same conn (deduplicated).
 */
void upstream_on_pending_phone(struct bt_conn *conn);

/*
 * upstream UART keepalive.
 *
 * Push the keepalive timer out by the full interval. Called by main on every
 * real ToRadio packet forwarded to the node, so the keepalive heartbeat only
 * actually fires after a stretch of true silence (it never adds traffic when
 * the link is already busy). Idempotent (k_work_reschedule rearms if pending).
 */
void upstream_keepalive_reschedule(void);

/*
 * keepalive queueStatus swallow.
 *
 * The node replies to our keepalive heartbeat with a queueStatus FromRadio.
 * That reply is for the proxy, not the phones, so router calls this when it
 * sees a queueStatus while LIVE: if a keepalive reply is outstanding it returns
 * true (consume the flag) and the caller drops the frame instead of
 * broadcasting it. Returns false for a genuine queueStatus (e.g. the node's
 * response to a phone's mesh packet), which is broadcast normally.
 *
 * Note: the keepalive only fires after ~5 min of silence, so a phone-packet
 * queueStatus is essentially never in flight when the flag is set.
 */
bool upstream_swallow_live_queuestatus(void);

/*
 * Recovery re-fetch. Re-issue the proxy's own want_config to reopen the node's
 * PhoneAPI session (which stops draining toPhoneQueue once it closes), consuming
 * and DISCARDING the resulting burst so the existing cache stays valid and
 * servable throughout. No-op if a fetch is already in progress. Called by the
 * reboot-signal handler and the liveness watchdog.
 */
void upstream_refetch(void);

#endif /* UPSTREAM_SESSION_H */
