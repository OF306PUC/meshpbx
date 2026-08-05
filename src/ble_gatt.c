/*
 * ble_gatt.c — Meshtastic-compatible GATT server
 *
 * Implements the official Meshtastic BLE service so the stock iOS/Android app
 * can connect as-is.  Supports MAX_BLE_CONNECTIONS simultaneous peripherals.
 *
 * Official Meshtastic BLE service UUIDs (from MeshtasticBleConstants.kt):
 *   Service   6ba1b218-15a8-461f-9fa8-5dcae273eafd
 *   FROMNUM   ed9da18c-a800-4f66-a670-aa7547e34453  (read + notify)
 *   FROMRADIO 2c55e69e-4993-11ed-b878-0242ac120002  (read)
 *   TORADIO   f75c76d2-129e-4dad-a1dd-7866124401e7  (write / write-no-rsp)
 *   LOGRADIO  5a3d6e49-06e6-4423-9944-e9de8cdf9547  (read + notify)
 *
 * Extra (custom) characteristic — ignored by the official app:
 *   NODE_REG  (write, 16 bytes) — phone writes its proxy_id (phone number,
 *             UUID, etc., null-padded to 16 bytes) to register for routing.
 *             A custom app calls this; the stock Meshtastic app skips it.
 *
 * GATT attribute table layout (meshtastic_svc.attrs[]):
 *   [0]  Primary service declaration
 *   [1]  FROMNUM characteristic declaration
 *   [2]  FROMNUM characteristic value        ← FROMNUM notify target
 *   [3]  FROMNUM CCC descriptor
 *   [4]  FROMRADIO characteristic declaration
 *   [5]  FROMRADIO characteristic value
 *   [6]  TORADIO characteristic declaration
 *   [7]  TORADIO characteristic value
 *   [8]  LOGRADIO characteristic declaration
 *   [9]  LOGRADIO characteristic value       ← LOGRADIO notify target
 *   [10] LOGRADIO CCC descriptor
 *   [11] NODE_REG characteristic declaration
 *   [12] NODE_REG characteristic value
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "ble_gatt.h"
#include "config_cache.h"
#include "proto_handler.h"
#include "route_trace.h"
#include "upstream_session.h"

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble_gatt, LOG_LEVEL_DBG);

/* ------------------------------------------------------------------ UUIDs */

#define BT_UUID_MESHTASTIC_SVC_VAL \
    BT_UUID_128_ENCODE(0x6ba1b218, 0x15a8, 0x461f, 0x9fa8, 0x5dcae273eafd)
#define BT_UUID_FROMNUM_VAL \
    BT_UUID_128_ENCODE(0xed9da18c, 0xa800, 0x4f66, 0xa670, 0xaa7547e34453)
#define BT_UUID_FROMRADIO_VAL \
    BT_UUID_128_ENCODE(0x2c55e69e, 0x4993, 0x11ed, 0xb878, 0x0242ac120002)
#define BT_UUID_TORADIO_VAL \
    BT_UUID_128_ENCODE(0xf75c76d2, 0x129e, 0x4dad, 0xa1dd, 0x7866124401e7)
#define BT_UUID_LOGRADIO_VAL \
    BT_UUID_128_ENCODE(0x5a3d6e49, 0x06e6, 0x4423, 0x9944, 0xe9de8cdf9547)
#define BT_UUID_NODE_REG_VAL \
    BT_UUID_128_ENCODE(0x6ba10001, 0x15a8, 0x461f, 0x9fa8, 0x5dcae273eafd)

static struct bt_uuid_128 uuid_meshtastic_svc = BT_UUID_INIT_128(BT_UUID_MESHTASTIC_SVC_VAL);
static struct bt_uuid_128 uuid_fromnum        = BT_UUID_INIT_128(BT_UUID_FROMNUM_VAL);
static struct bt_uuid_128 uuid_fromradio      = BT_UUID_INIT_128(BT_UUID_FROMRADIO_VAL);
static struct bt_uuid_128 uuid_toradio        = BT_UUID_INIT_128(BT_UUID_TORADIO_VAL);
static struct bt_uuid_128 uuid_logradio       = BT_UUID_INIT_128(BT_UUID_LOGRADIO_VAL);
static struct bt_uuid_128 uuid_node_reg       = BT_UUID_INIT_128(BT_UUID_NODE_REG_VAL);

/* Blinky LED assignments (BLE events): led0=FromRadio, led1=ToRadio */
#define LED0_NODE DT_ALIAS(led0) /* FromRadio messages */
#define LED1_NODE DT_ALIAS(led1) /* To Radio messages */

static const struct gpio_dt_spec led_ble_fromradio = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led_ble_toradio   = GPIO_DT_SPEC_GET(LED1_NODE, gpios);

/* Activity LEDs blink while BLE traffic is flowing */
#define LED_BLINK_PERIOD K_MSEC(120)

static atomic_t led_fromradio_active; /* FromRadio activity (FROMNUM polls) */
static atomic_t led_toradio_active;   /* ToRadio activity (writes) */

static void led_blink_tick(const struct gpio_dt_spec *led, atomic_t *active)
{
    if (atomic_set(active, 0) != 0) {
        (void)gpio_pin_toggle_dt(led); /* traffic since last tick: blink */
    } else {
        (void)gpio_pin_set_dt(led, 0); /* idle: hold off */
    }
}

static void led_blink_work(struct k_work *work)
{
    ARG_UNUSED(work);
    led_blink_tick(&led_ble_fromradio, &led_fromradio_active);
    led_blink_tick(&led_ble_toradio, &led_toradio_active);
    k_work_reschedule(k_work_delayable_from_work(work), LED_BLINK_PERIOD);
}

static K_WORK_DELAYABLE_DEFINE(led_blink_dwork, led_blink_work);

/* Attribute indices for notify targets (see layout comment at top of file) */
#define FROMNUM_ATTR_IDX  2
#define LOGRADIO_ATTR_IDX 9

/* --------------------------------------------------------- Per-conn state */

struct pkt_entry {
    uint8_t  data[FROMRADIO_MAX_PKT_SIZE];
    uint16_t len;
};

struct proxy_conn {
    struct bt_conn  *conn;
    proxy_id_t       proxy_id;       /* Phone identifier registered via NODE_REG    */
    uint32_t         fromnum;        /* Monotonically incrementing packet counter   */

    /* Per-phone config-session state */
    enum phone_state state;          /* CONNECTED → … → ACTIVE                       */
    uint32_t         nonce;          /* This phone's want_config nonce               */
    uint16_t         replay_cursor;  /* Next cache frame index to replay             */
    uint8_t          cc_frame[16];   /* Pre-encoded config_complete_id (served last) */
    uint8_t          cc_len;         /* Length of cc_frame; 0 until replay armed      */

    /* Circular queue of outbound FromRadio packets */
    struct pkt_entry queue[FROMRADIO_QUEUE_DEPTH];
    uint8_t          head;
    uint8_t          tail;
    uint8_t          count;

    /* Staging buffer: the packet currently being read via ATT (handles long reads) */
    uint8_t          staged[FROMRADIO_MAX_PKT_SIZE];
    uint16_t         staged_len;

    struct k_mutex   lock;
};

static struct proxy_conn conns[MAX_BLE_CONNECTIONS];
static uint8_t           active_conn_count;
static toradio_cb_t      toradio_handler;

/* --------------------------------------------------- Connection utilities */

static struct proxy_conn *find_slot(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (conns[i].conn == conn) {
            return &conns[i];
        }
    }
    return NULL;
}

static struct proxy_conn *alloc_slot(struct bt_conn *conn)
{
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (conns[i].conn == NULL) {
            conns[i].conn          = bt_conn_ref(conn);
            memset(&conns[i].proxy_id, 0, sizeof(proxy_id_t));
            conns[i].fromnum       = 0;
            conns[i].head          = 0;
            conns[i].tail          = 0;
            conns[i].count         = 0;
            conns[i].staged_len    = 0;
            conns[i].state         = PHONE_CONNECTED;
            conns[i].nonce         = 0;
            conns[i].replay_cursor = 0;
            return &conns[i];
        }
    }
    return NULL;
}

static void free_slot(struct proxy_conn *pc)
{
    if (pc->conn) {
        bt_conn_unref(pc->conn);
        pc->conn = NULL;
    }
    memset(&pc->proxy_id, 0, sizeof(proxy_id_t));
    pc->fromnum       = 0;
    pc->head          = 0;
    pc->tail          = 0;
    pc->count         = 0;
    pc->staged_len    = 0;
    pc->state         = PHONE_CONNECTED;
    pc->nonce         = 0;
    pc->replay_cursor = 0;
}

/* --------------------------------------------------------- GATT callbacks */

/* FROMNUM read — returns the per-connection packet counter. */
static ssize_t fromnum_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    /* Mark FromRadio activity; the blink worker drives the LED. */
    atomic_set(&led_fromradio_active, 1);

    uint32_t val = pc->fromnum;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &val, sizeof(val));
}

/* FROMNUM CCC changed — logged for debugging; Zephyr tracks subscription internally. */
static void fromnum_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("FROMNUM CCC: %s",
            (value == BT_GATT_CCC_NOTIFY) ? "notify enabled" : "notify disabled");
}

/*
 * FROMRADIO read — serves the next queued FromRadio packet for this connection.
 *
 * Protocol (mirrors official Meshtastic BLE behavior):
 *   - Phone subscribes to FROMNUM notifications.
 *   - On notification, phone reads FROMRADIO repeatedly until the response
 *     is empty (len == 0), draining the queue.
 *   - Each fresh read (offset == 0) stages the next queued packet.
 *   - Subsequent reads at offset > 0 continue serving the staged packet
 *     (ATT long-read for packets > MTU - 1).
 */
static ssize_t fromradio_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }

    k_mutex_lock(&pc->lock, K_FOREVER);

    if (offset == 0) {
        /* Fresh read: stage the next packet (or mark empty). */
        if (pc->state == PHONE_REPLAYING) {
            /*
             * Serve-on-read replay: hand back ONE cached config frame per
             * ATT read, straight from the shared arena, then the synthesized
             * config_complete_id, then go ACTIVE. This keeps RAM at O(1) per conn
             * (no pre-enqueue of a dozens-of-frames burst into the 8-deep queue).
             */
            uint16_t n = cache_frame_count();
            /* Skip cached frames that are not in this phone's nonce segment
             * (69420 ONLY_CONFIG / 69421 ONLY_NODES subsets; config_complete is
             * always skipped here and synthesized below). */
            while (pc->replay_cursor < n &&
                   !cache_frame_in_segment(pc->replay_cursor, pc->nonce)) {
                pc->replay_cursor++;
            }
            if (pc->replay_cursor < n) {
                const struct cache_frame_ref *ref = cache_frame_at(pc->replay_cursor);
                if (ref) {
                    memcpy(pc->staged, cache_frame_bytes(ref), ref->len);
                    pc->staged_len = ref->len;
                } else {
                    pc->staged_len = 0;
                }
                pc->replay_cursor++;
            } else if (pc->replay_cursor == n) {
                /* All cache frames served — emit config_complete_id last. */
                memcpy(pc->staged, pc->cc_frame, pc->cc_len);
                pc->staged_len = pc->cc_len;
                pc->replay_cursor++;
            } else {
                /* Burst fully drained — go live and report empty. */
                pc->state      = PHONE_ACTIVE;
                pc->staged_len = 0;
                LOG_INF("replay drained → conn %p ACTIVE", (void *)conn);
            }
        } else if (pc->count > 0) {
            struct pkt_entry *pkt = &pc->queue[pc->head];
            memcpy(pc->staged, pkt->data, pkt->len);
            pc->staged_len = pkt->len;
            pc->head       = (pc->head + 1) % FROMRADIO_QUEUE_DEPTH;
            pc->count--;
        } else {
            pc->staged_len = 0;  /* Empty response signals queue drained. */
        }
    }

    ssize_t result;
    if (pc->staged_len > 0) {
        result = bt_gatt_attr_read(conn, attr, buf, len, offset,
                                   pc->staged, pc->staged_len);
    } else {
        result = 0;  /* Empty: Meshtastic app stops reading. */
    }

    k_mutex_unlock(&pc->lock);
    return result;
}

/* LOGRADIO read — returns empty for now; Phase 2 can forward Meshtastic node logs. */
static ssize_t logradio_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              void *buf, uint16_t len, uint16_t offset)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(buf);
    ARG_UNUSED(len);
    ARG_UNUSED(offset);
    return 0;  /* Empty: no log data staged yet */
}

static void logradio_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    LOG_DBG("LOGRADIO CCC: %s",
            (value == BT_GATT_CCC_NOTIFY) ? "notify enabled" : "notify disabled");
}

/* TORADIO write — raw protobuf bytes from phone, forwarded to UART in Phase 2. */
static ssize_t toradio_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset,
                              uint8_t flags)
{
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    /* Mark ToRadio activity; the blink worker drives the LED. */
    atomic_set(&led_toradio_active, 1);

    if (toradio_handler) {
        toradio_handler(conn, buf, len);
    }
    return len;
}

/*
 * NODE_REG write — phone writes its 4-byte proxy_id (phone number, UUID, etc.,
 * null-padded). Ignored by the official Meshtastic app; used by custom apps.
 */
static ssize_t node_reg_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset,
                               uint8_t flags)
{
    if (offset != 0 || len != PROXY_ID_SIZE) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    proxy_id_t id;
    memcpy(id.bytes, buf, PROXY_ID_SIZE);
    ble_gatt_register_proxy_id(conn, &id);
    return len;
}

/* ------------------------------------------------------- GATT service def */

BT_GATT_SERVICE_DEFINE(meshtastic_svc,
    BT_GATT_PRIMARY_SERVICE(&uuid_meshtastic_svc),

    /* FROMNUM — read + notify (phone polls packet count) */
    BT_GATT_CHARACTERISTIC(&uuid_fromnum.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ,
        fromnum_read, NULL, NULL),
    BT_GATT_CCC(fromnum_ccc_changed,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* FROMRADIO — read (phone drains packet queue) */
    BT_GATT_CHARACTERISTIC(&uuid_fromradio.uuid,
        BT_GATT_CHRC_READ,
        BT_GATT_PERM_READ,
        fromradio_read, NULL, NULL),

    /* TORADIO — write (phone sends ToRadio protobuf) */
    BT_GATT_CHARACTERISTIC(&uuid_toradio.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, toradio_write, NULL),

    /* LOGRADIO — read + notify (device log output; stub for Phase 1) */
    BT_GATT_CHARACTERISTIC(&uuid_logradio.uuid,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ,
        logradio_read, NULL, NULL),
    BT_GATT_CCC(logradio_ccc_changed,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    /* NODE_REG — write (custom: phone registers its node_id for routing) */
    BT_GATT_CHARACTERISTIC(&uuid_node_reg.uuid,
        BT_GATT_CHRC_WRITE,
        BT_GATT_PERM_WRITE,
        NULL, node_reg_write, NULL),
);

/* -------------------------------------------------- Connection callbacks */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %d)", err);
        return;
    }

    struct proxy_conn *pc = alloc_slot(conn);
    if (!pc) {
        LOG_ERR("No free connection slot — rejecting");
        bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    active_conn_count++;
    LOG_INF("Connected [slot %ld] — active: %d/%d",
            (long)(pc - conns), active_conn_count, MAX_BLE_CONNECTIONS);

    /* Keep advertising so the second phone can also connect. */
    if (active_conn_count < MAX_BLE_CONNECTIONS) {
        int adv_err = ble_gatt_start_advertising();
        if (adv_err) {
            LOG_ERR("Re-advertising after connect failed: %d — second slot unavailable", adv_err);
        }
    }
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    struct proxy_conn *pc = find_slot(conn);
    if (pc) {
        LOG_INF("Disconnected [slot %ld] (reason 0x%02x)",
                (long)(pc - conns), reason);
        free_slot(pc);
        active_conn_count--;
    }

    /* Restart advertising to accept a new connection. */
    ble_gatt_start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ------------------------------------------------------ Advertising data */

/*
 * AD: flags + 128-bit Meshtastic service UUID.
 * The Meshtastic iOS/Android app scans for this UUID to discover proxy devices.
 */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_MESHTASTIC_SVC_VAL),
};

/* SD: full device name. Meshtastic app also filters on "Meshtastic" prefix. */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ------------------------------------------------------ Public API */

int ble_gatt_start_advertising(void)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err && err != -EALREADY) {
        LOG_ERR("Advertising failed to start: %d", err);
        return err;
    }
    LOG_INF("Advertising as '%s'", CONFIG_BT_DEVICE_NAME);
    return 0;
}

int ble_gatt_init(toradio_cb_t cb)
{
    /* Activity LEDs: start off, toggle on BLE read/write events. */
    if (gpio_is_ready_dt(&led_ble_fromradio) && gpio_is_ready_dt(&led_ble_toradio)) {
        int ret = gpio_pin_configure_dt(&led_ble_fromradio, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("LED0 failed to be configured: %d", ret);
        }

        ret = gpio_pin_configure_dt(&led_ble_toradio, GPIO_OUTPUT_INACTIVE);
        if (ret < 0) {
            LOG_ERR("LED1 failed to be configured: %d", ret);
        }

        k_work_schedule(&led_blink_dwork, LED_BLINK_PERIOD);
    }

    toradio_handler = cb;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        k_mutex_init(&conns[i].lock);
    }
    return 0;
}

int ble_gatt_enqueue_fromradio(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    if (len > FROMRADIO_MAX_PKT_SIZE) {
        return -EMSGSIZE;
    }

    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        return -ENOENT;
    }

    k_mutex_lock(&pc->lock, K_FOREVER);

    if (pc->count >= FROMRADIO_QUEUE_DEPTH) {
        k_mutex_unlock(&pc->lock);
        LOG_WRN("FromRadio queue full for conn %p", (void *)conn);
        return -ENOMEM;
    }

    struct pkt_entry *slot = &pc->queue[pc->tail];
    memcpy(slot->data, data, len);
    slot->len  = len;
    pc->tail   = (pc->tail + 1) % FROMRADIO_QUEUE_DEPTH;
    pc->count++;
    pc->fromnum++;

    uint32_t fromnum_val = pc->fromnum;
    k_mutex_unlock(&pc->lock);

    /* Notify the routed contact (phone). Zephyr checks CCC internally and drops if not subscribed. */
    int err = bt_gatt_notify(conn, &meshtastic_svc.attrs[FROMNUM_ATTR_IDX],
                             &fromnum_val, sizeof(fromnum_val));
    
    if (err) {
        LOG_WRN("FROMNUM notify failed: %d", err);
    } else {
        LOG_INF("FROMNUM notification to=+56%" PRIu32 " fromnum=%" PRIu32,
                sys_get_be32(pc->proxy_id.bytes), fromnum_val);
    }

    return 0;
}

struct bt_conn *ble_gatt_get_conn_by_proxy_id(const proxy_id_t *id)
{
    if (proxy_id_is_zero(id)) {
        return NULL;
    }
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (conns[i].conn && proxy_id_equal(&conns[i].proxy_id, id)) {
            return conns[i].conn;
        }
    }
    return NULL;
}

int ble_gatt_register_proxy_id(struct bt_conn *conn, const proxy_id_t *id)
{
    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        return -ENOENT;
    }
    memcpy(&pc->proxy_id, id, sizeof(proxy_id_t));
    uint32_t phone_num_reg = sys_get_be32(id->bytes); 
    LOG_INF("proxy_id [CL phone]: +56%" PRIu32 ". Register for slot %ld",
            phone_num_reg, (long)(pc - conns));
    return 0;
}

int ble_gatt_broadcast_fromradio(const uint8_t *data, uint16_t len)
{
    int sent = 0;
    for (int i = 0; i < MAX_BLE_CONNECTIONS; i++) {
        if (conns[i].conn) {
            int err = ble_gatt_enqueue_fromradio(conns[i].conn, data, len);
            if (err) {
                LOG_WRN("broadcast: slot %d enqueue failed: %d", i, err);
            } else {
                sent++;
            }
        }
    }
    return sent;  /* number of connections that received the packet */
}

/* ------------------------------------------------ Per-phone config replay */

void ble_gatt_replay_cached_burst(struct bt_conn *conn, uint32_t nonce)
{
    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        LOG_WRN("replay: conn %p not tracked", (void *)conn);
        return;
    }

    /*
     * Two entry paths share this function:
     *   - cache already ready: on_toradio_ble calls with the phone's live nonce.
     *   - PENDING served later: upstream's serve callback fires here; the phone's
     *     nonce was stored by ble_gatt_park_pending() (upstream tracks only conn,
     *     not the nonce). A passed nonce of 0 means "use the stored one".
     */
    if (nonce != 0) {
        pc->nonce = nonce;
    }
    nonce = pc->nonce;

    /*
     * We do NOT pre-enqueue the burst: a real want_config burst is 
     * dozens of frames and would overflow the 8-deep
     * per-conn queue (frames 9+ would be dropped mid-handshake). Instead we
     * pre-encode the trailing config_complete_id once, arm a cursor, and let
     * fromradio_read() serve one cached frame per ATT read straight from the
     * shared arena. The phone drains FROMRADIO until an empty reply, so a
     * single FROMNUM kick walks the entire virtual burst with O(1) RAM here.
     */
    uint16_t cc_len = 0;
    int enc = proto_encode_config_complete(nonce, pc->cc_frame, sizeof(pc->cc_frame),
                                           &cc_len);
    if (enc != 0) {
        LOG_ERR("replay: config_complete encode failed: %d — replay aborted", enc);
        return;
    }

    k_mutex_lock(&pc->lock, K_FOREVER);
    pc->cc_len        = (uint8_t)cc_len;
    pc->replay_cursor = 0;
    pc->state         = PHONE_REPLAYING;
    pc->fromnum++;
    uint32_t fromnum_val = pc->fromnum;
    k_mutex_unlock(&pc->lock);

    /* Kick the phone to start its FROMRADIO drain loop. */
    int nerr = bt_gatt_notify(conn, &meshtastic_svc.attrs[FROMNUM_ATTR_IDX],
                              &fromnum_val, sizeof(fromnum_val));
    if (nerr) {
        LOG_WRN("replay: FROMNUM kick failed: %d", nerr);
    }

    LOG_INF("replay armed: %u cache frames + config_complete (nonce=%u) → conn %p",
            cache_frame_count(), (unsigned)nonce, (void *)conn);
}

void ble_gatt_park_pending(struct bt_conn *conn, uint32_t nonce)
{
    struct proxy_conn *pc = find_slot(conn);
    if (!pc) {
        LOG_WRN("park_pending: conn %p not tracked", (void *)conn);
        return;
    }

    pc->state = PHONE_PENDING;
    pc->nonce = nonce;
    upstream_on_pending_phone(conn);
    LOG_INF("park_pending: conn %p queued (nonce=%u) until cache ready",
            (void *)conn, (unsigned)nonce);
}

void ble_gatt_reply_queuestatus(struct bt_conn *conn)
{
    /* The synthesized queueStatus is constant — encode it once and reuse.
     * Encoded here (BT RX context) on first use; toradio_write callbacks are
     * serialized by the BT stack, so the lazy init needs no extra lock. */
    static uint8_t  s_qs[16];
    static uint16_t s_qs_len;

    if (s_qs_len == 0) {
        int rc = proto_encode_queue_status(s_qs, sizeof(s_qs), &s_qs_len);
        if (rc != 0) {
            LOG_ERR("synth queueStatus encode failed: %d", rc);
            s_qs_len = 0;
            return;
        }
    }

    int err = ble_gatt_enqueue_fromradio(conn, s_qs, s_qs_len);
    if (err) {
        LOG_WRN("queueStatus reply enqueue failed: %d (conn %p)", err, (void *)conn);
    } else {
        LOG_DBG("heartbeat → synth queueStatus (%u B) → conn %p",
                (unsigned)s_qs_len, (void *)conn);
    }
}
