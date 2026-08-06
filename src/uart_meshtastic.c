/*
 * uart_meshtastic.c — UART driver for Meshtastic serial API
 *
 * Uses Zephyr async UART (DMA-backed) on UART1.
 *
 * RX path: ISR → ring_buf → rx_work → state machine → fromradio_cb()
 *
 * TX path: caller → k_msgq (TX queue) → tx_work → uart_tx()
 *          UART_TX_DONE ISR → tx_work (drain next entry)
 *
 * The TX queue (TX_QUEUE_DEPTH entries) absorbs bursts from multiple phones
 * writing TORADIO simultaneously. Entries are sent strictly in FIFO order.
 *
 * Framing: [0x94][0xC3][len_hi][len_lo][protobuf_payload]
 */

#include "uart_meshtastic.h"
#include "route_trace.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>

LOG_MODULE_REGISTER(uart_meshtastic, LOG_LEVEL_DBG);

/* ---------------------------------------------------------------- Constants */

#define MESHTASTIC_MAGIC1    0x94U
#define MESHTASTIC_MAGIC2    0xC3U

#define UART_DMA_BUF_SIZE    256U   /* DMA double-buffer size per buffer        */
#define RING_BUF_SIZE        2048U  /* RX ring buffer (power of 2)              */
#define UART_RX_TIMEOUT_US   2000U  /* Inter-byte timeout before UART_RX_RDY    */
#define TX_QUEUE_DEPTH       4U     /* Max queued outbound ToRadio packets      */
#define RX_RESYNC_TIMEOUT_MS 250U   /* > max frame time (512 bytes --> ~45ms); reset a stalled partial frame */

/*
 * TX timeout: guards against a stuck UART hardware (not receiver backpressure —
 * UART without flow control cannot be stalled by the remote side).
 *
 * Derived from worst-case frame size and baud rate:
 *   byte time = 10 (byte-protocol) bits (8N1) / 115200 ≈ 87 µs
 *   max frame = 4 (header) + MESHTASTIC_MAX_PAYLOAD bytes
 *   max TX time ≈ 516 × 87 µs ≈ 45 ms
 *   2× safety margin → ~90 ms
 *
 * On UART_TX_ABORTED: tx_in_progress is cleared and the next queued packet
 * is attempted. The aborted packet is discarded.
 */
#define UART_BITS_PER_BYTE   10U    /* 8N1: 1 start + 8 data + 1 stop          */
#define UART_BAUD            115200U
#define UART_US_PER_BYTE     ((UART_BITS_PER_BYTE * 1000000U) / UART_BAUD) /* ~87 */
#define TX_TIMEOUT_US        ((MESHTASTIC_MAX_PAYLOAD + 4U) * UART_US_PER_BYTE * 2U)

/* ----------------------------------------------------------- TX queue entry */

struct tx_entry {
    uint8_t  payload[MESHTASTIC_MAX_PAYLOAD];
    uint16_t len;
};

/* k_msgq: thread-safe FIFO for outbound packets.
 * Each entry is a full tx_entry (payload copy + length). */
K_MSGQ_DEFINE(tx_msgq, sizeof(struct tx_entry), TX_QUEUE_DEPTH, 4);

/* TX frame buffer: written only inside tx_work_handler (single work queue). */
static uint8_t tx_frame_buf[MESHTASTIC_MAX_PAYLOAD + 4];  /* header + payload */

/* True while uart_tx() has ownership of tx_frame_buf (until UART_TX_DONE). */
static volatile bool tx_in_progress;

/* ----------------------------------------------------------------- RX side */

static uint8_t dma_buf[2][UART_DMA_BUF_SIZE];
static uint8_t active_buf;

RING_BUF_DECLARE(rx_ring_buf, RING_BUF_SIZE);

/* ----------------------------------------------------------------- Statics */

static const struct device  *uart_dev;
static fromradio_uart_cb_t   fromradio_cb;
static atomic_t              rx_overrun_bytes;  /* bytes dropped by a full RX ring */

/* ------------------------------------------------------------- Activity LEDs */

/* Blink while UART traffic is flowing: rx=led2, tx=led3 */
#define LED_RX_NODE DT_ALIAS(led2) /* UART RX (from Meshtastic) */
#define LED_TX_NODE DT_ALIAS(led3) /* UART TX (to Meshtastic)   */

#define LED_BLINK_PERIOD K_MSEC(120)

static const struct gpio_dt_spec led_uart_rx = GPIO_DT_SPEC_GET(LED_RX_NODE, gpios);
static const struct gpio_dt_spec led_uart_tx = GPIO_DT_SPEC_GET(LED_TX_NODE, gpios);

static atomic_t uart_rx_active;
static atomic_t uart_tx_active;

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
    led_blink_tick(&led_uart_rx, &uart_rx_active);
    led_blink_tick(&led_uart_tx, &uart_tx_active);
    k_work_reschedule(k_work_delayable_from_work(work), LED_BLINK_PERIOD);
}

static K_WORK_DELAYABLE_DEFINE(led_blink_dwork, led_blink_work);

/* --------------------------------------------------------- RX state machine */

enum rx_state {
    RX_WAIT_MAGIC1,
    RX_WAIT_MAGIC2,
    RX_WAIT_LEN_HI,
    RX_WAIT_LEN_LO,
    RX_WAIT_PAYLOAD,
};

static struct {
    enum rx_state state;
    uint16_t      expected_len;
    uint16_t      received_len;
    uint8_t       payload[MESHTASTIC_MAX_PAYLOAD];
} rx_sm;

static void rx_process_byte(uint8_t byte)
{
    switch (rx_sm.state) {

    case RX_WAIT_MAGIC1:
        if (byte == MESHTASTIC_MAGIC1) {
            rx_sm.state = RX_WAIT_MAGIC2;
        }
        break;

    case RX_WAIT_MAGIC2:
        if (byte == MESHTASTIC_MAGIC2) {
            rx_sm.state = RX_WAIT_LEN_HI;
        } else {
            rx_sm.state = (byte == MESHTASTIC_MAGIC1) ? RX_WAIT_MAGIC2
                                                       : RX_WAIT_MAGIC1;
        }
        break;

    case RX_WAIT_LEN_HI:
        rx_sm.expected_len = (uint16_t)byte << 8;
        rx_sm.state = RX_WAIT_LEN_LO;
        break;

    case RX_WAIT_LEN_LO:
        rx_sm.expected_len |= byte;
        if (rx_sm.expected_len == 0 ||
            rx_sm.expected_len > MESHTASTIC_MAX_PAYLOAD) {
            LOG_WRN("Bad frame length %d — resyncing", rx_sm.expected_len);
            rx_sm.state = RX_WAIT_MAGIC1;
        } else {
            rx_sm.received_len = 0;
            rx_sm.state = RX_WAIT_PAYLOAD;
        }
        break;

    case RX_WAIT_PAYLOAD:
        rx_sm.payload[rx_sm.received_len++] = byte;
        if (rx_sm.received_len == rx_sm.expected_len) {
            if (fromradio_cb) {
                fromradio_cb(rx_sm.payload, rx_sm.received_len);
            }
            rx_sm.state = RX_WAIT_MAGIC1;
        }
        break;
    }
}

/* Reset a stalled partial frame. Runs on the system work queue (same context as
 * rx_work_handler), so it touches rx_sm without a mutex. */
static void rx_resync_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    if (rx_sm.state != RX_WAIT_MAGIC1) {
        LOG_WRN("RX frame stalled mid-frame (state=%u, %u/%u B) — resyncing",
                rx_sm.state, rx_sm.received_len, rx_sm.expected_len);
        rx_sm.state        = RX_WAIT_MAGIC1;
        rx_sm.received_len = 0;
        rx_sm.expected_len = 0;
    }
}

static K_WORK_DELAYABLE_DEFINE(rx_resync_dwork, rx_resync_handler);

static void rx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    uint8_t byte;
    while (ring_buf_get(&rx_ring_buf, &byte, 1) == 1) {
        rx_process_byte(byte);
    }

    /* No-progress watchdog: reschedule (push the deadline) while mid-frame,
     * cancel at a frame boundary. Fires only after a true mid-frame stall. */
    if (rx_sm.state != RX_WAIT_MAGIC1) {
        k_work_reschedule(&rx_resync_dwork, K_MSEC(RX_RESYNC_TIMEOUT_MS));
    } else {
        k_work_cancel_delayable(&rx_resync_dwork);
    }

    atomic_val_t dropped = atomic_set(&rx_overrun_bytes, 0);
    if (dropped > 0) {
        LOG_WRN("RX overrun: %u byte(s) dropped (ring full)", (unsigned)dropped);
    }
}

static K_WORK_DEFINE(rx_work, rx_work_handler);
/* ------------------------------------------------------ TX work handler
 *
 * Dequeues one entry from tx_msgq and starts a uart_tx().
 * Re-submitted by UART_TX_DONE to drain the next entry (if any).
 * Guard: tx_in_progress prevents double-starting uart_tx().
 */
static void tx_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (tx_in_progress) {
        /* A TX is already running; UART_TX_DONE will re-submit us. */
        return;
    }

    struct tx_entry entry;
    if (k_msgq_get(&tx_msgq, &entry, K_NO_WAIT) != 0) {
        return;  /* Queue empty — nothing to send. */
    }

    /* Build Meshtastic frame into the static frame buffer. */
    tx_frame_buf[0] = MESHTASTIC_MAGIC1;
    tx_frame_buf[1] = MESHTASTIC_MAGIC2;
    tx_frame_buf[2] = (uint8_t)(entry.len >> 8);
    tx_frame_buf[3] = (uint8_t)(entry.len & 0xFFU);
    memcpy(&tx_frame_buf[4], entry.payload, entry.len);

    tx_in_progress = true;
    int err = uart_tx(uart_dev, tx_frame_buf, (size_t)(entry.len + 4U),
                      TX_TIMEOUT_US);
    if (err) {
        LOG_ERR("uart_tx: %d", err);
        tx_in_progress = false;
        /* Drop this entry and try the next one. */
        k_work_submit(work);
    }
}

static K_WORK_DEFINE(tx_work, tx_work_handler);

/* ------------------------------------------------------------ UART callback */

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
    ARG_UNUSED(user_data);

    switch (evt->type) {

    case UART_RX_RDY:
        /* Data ready: either the DMA buffer filled, OR (with HW-async byte
         * counting enabled) the RX line went idle for UART_RX_TIMEOUT_US and the
         * driver flushed a partial buffer. The latter is what delivers a lone
         * FromRadio frame promptly instead of stalling until the buffer fills. */
        uint32_t put = ring_buf_put(&rx_ring_buf,
                                    evt->data.rx.buf + evt->data.rx.offset,
                                    evt->data.rx.len);
        if (put < evt->data.rx.len) {
            atomic_add(&rx_overrun_bytes, (atomic_val_t)(evt->data.rx.len - put));
        }
        atomic_set(&uart_rx_active, 1);
        k_work_submit(&rx_work);
        break;

    case UART_RX_BUF_REQUEST:
        active_buf ^= 1U;
        uart_rx_buf_rsp(dev, dma_buf[active_buf], UART_DMA_BUF_SIZE);
        break;

    case UART_RX_BUF_RELEASED:
        break;

    case UART_RX_STOPPED:
        /* RX halted on a bus error (framing / parity / overrun). RX_DISABLED
         * follows and re-arms reception there. */
        LOG_WRN("UART RX stopped (reason 0x%x) — will re-enable",
                (unsigned)evt->data.rx_stop.reason);
        break;

    case UART_RX_DISABLED: {
        /* Reception ended (after an error/stop). Re-arm so the link recovers
         * instead of going silently deaf. */
        active_buf = 0U;
        int rerr = uart_rx_enable(dev, dma_buf[active_buf], UART_DMA_BUF_SIZE,
                                  UART_RX_TIMEOUT_US);
        if (rerr) {
            LOG_ERR("UART RX re-enable failed: %d", rerr);
        }
        rx_sm.state = RX_WAIT_MAGIC1;
        break;
    }

    case UART_TX_DONE:
        tx_in_progress = false;
        atomic_set(&uart_tx_active, 1);
        /* Drain the next queued entry, if any. */
        k_work_submit(&tx_work);
        break;

    case UART_TX_ABORTED:
        LOG_ERR("UART TX aborted");
        tx_in_progress = false;
        k_work_submit(&tx_work);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------- Public API */

int uart_meshtastic_init(fromradio_uart_cb_t cb)
{
    fromradio_cb   = cb;
    rx_sm.state    = RX_WAIT_MAGIC1;
    tx_in_progress = false;

    /* Activity LEDs: start off, toggle while UART traffic is flowing. */
    if (gpio_is_ready_dt(&led_uart_rx) && gpio_is_ready_dt(&led_uart_tx)) {
        int led_err = gpio_pin_configure_dt(&led_uart_rx, GPIO_OUTPUT_INACTIVE);
        if (led_err < 0) {
            LOG_ERR("UART RX LED (led2) configure failed: %d", led_err);
        }

        led_err = gpio_pin_configure_dt(&led_uart_tx, GPIO_OUTPUT_INACTIVE);
        if (led_err < 0) {
            LOG_ERR("UART TX LED (led3) configure failed: %d", led_err);
        }

        k_work_schedule(&led_blink_dwork, LED_BLINK_PERIOD);
    }

    uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART1 not ready — check overlay");
        return -ENODEV;
    }

    int err = uart_callback_set(uart_dev, uart_cb, NULL);
    if (err) {
        LOG_ERR("uart_callback_set: %d", err);
        return err;
    }

    active_buf = 0;
    err = uart_rx_enable(uart_dev, dma_buf[0], UART_DMA_BUF_SIZE,
                         UART_RX_TIMEOUT_US);
    if (err) {
        LOG_ERR("uart_rx_enable: %d", err);
        return err;
    }

    LOG_INF("UART1 ready — 115200 baud, P1.01 RX / P1.02 TX, TX queue depth %d",
            TX_QUEUE_DEPTH);
    return 0;
}

int uart_meshtastic_tx(const uint8_t *payload, uint16_t len)
{
    if (len > MESHTASTIC_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }

    struct tx_entry entry;
    memcpy(entry.payload, payload, len);
    entry.len = len;

    /* k_msgq_put is thread-safe; K_NO_WAIT returns -ENOMEM if queue is full. */
    int err = k_msgq_put(&tx_msgq, &entry, K_NO_WAIT);
    if (err) {
        LOG_WRN("TX queue full (%d slots) — ToRadio dropped", TX_QUEUE_DEPTH);
        return -ENOMEM;
    }

    /* Kick the TX work. Idempotent if it's already queued or running. */
    k_work_submit(&tx_work);
    return 0;
}
