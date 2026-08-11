#ifndef PBX_STATS_H
#define PBX_STATS_H

#include <stdint.h>

/*
 * Cumulative counters, dumped periodically as one log line.
 *
 * WHY CUMULATIVE AND NOT EVENTS
 *
 * The individual drop sites already log a warning each time they fire, which is
 * right for a human reading the console but useless for measurement: the
 * collector on the host parses this log, and Zephyr drops log messages when its
 * buffer fills. A missed "dropped" warning is a loss that is undercounted
 * FOREVER, silently, and the resulting delivery ratio is simply wrong with
 * nothing anywhere to indicate it.
 *
 * A running total is self-healing instead. Every dump carries the complete
 * state, so a dropped line costs nothing — the next line still reports the
 * correct figure. Stock Meshtastic already does this on its own console
 * (`txGood=…,txRelay=…,rxGood=…,rxBad=…`), which is what made the host-side
 * parser for that device reliable; this brings the proxy up to the same standard.
 *
 * One line holds every counter on purpose. Splitting them across lines would
 * reintroduce the problem for whichever line went missing.
 *
 * Counters are monotonic and never reset except by reboot, which the host
 * detects from the uptime in the log timestamp going backwards.
 */

enum pbx_stat {
	/* ── Uplink: phone → proxy → node ─────────────────────────────────── */
	PBX_STAT_TX_FWD,          /* ToRadio frames handed to the UART           */
	PBX_STAT_TX_DROP_TXQ,     /* dropped: UART TX queue full                 */
	PBX_STAT_TX_DROP_ERR,     /* dropped: uart_meshtastic_tx returned error   */
	PBX_STAT_TX_DECODE_FAIL,  /* ToRadio would not decode; forwarded raw     */

	/* ── Downlink: node → proxy → phone ───────────────────────────────── */
	PBX_STAT_RX_FRAMES,       /* complete FromRadio frames off the UART      */
	PBX_STAT_RX_OVERRUN_B,    /* bytes lost to a full RX ring                */
	PBX_STAT_RX_RESYNC,       /* mid-frame stalls and bad lengths, resynced  */
	PBX_STAT_RX_DECODE_FAIL,  /* FromRadio would not decode; broadcast raw   */

	/* ── Routing ──────────────────────────────────────────────────────── */
	PBX_STAT_ROUTED,          /* delivered to one phone by DST_ID            */
	PBX_STAT_BROADCAST,       /* delivered to every connection               */
	PBX_STAT_BCAST_FALLBACK,  /* wanted to route, could not: bad hdr or
	                             DST_ID not registered                       */
	PBX_STAT_PHONE_Q_DROP,    /* dropped: a phone's FromRadio queue was full */

	/* ── BLE session ──────────────────────────────────────────────────── */
	PBX_STAT_BLE_CONNECT,
	PBX_STAT_BLE_DISCONNECT,
	PBX_STAT_BLE_REJECT,      /* refused: no free connection slot            */
	PBX_STAT_NODE_REG,        /* phones that registered a proxy_id           */

	/* ── Upstream session health ──────────────────────────────────────── */
	PBX_STAT_NODE_REBOOT,     /* node restarts the proxy noticed             */
	PBX_STAT_SESSION_REFETCH, /* want_config re-issued after presumed death  */
	PBX_STAT_KEEPALIVE_FAIL,

	PBX_STAT_COUNT
};

/** Increments one counter. Safe from any context; never blocks. */
void pbx_stats_inc(enum pbx_stat stat);

/** Adds to one counter, for byte counts rather than occurrences. */
void pbx_stats_add(enum pbx_stat stat, uint32_t delta);

/**
 * Starts the periodic dump. Call once from main() after logging is up.
 *
 * The period is CONFIG_MESHTASTIC_STATS_PERIOD_S. It should be short relative to
 * how long a measurement run lasts and long relative to the log buffer's drain
 * rate; 30 s matches the gateway receiver's PDR sweep interval.
 */
void pbx_stats_start(void);

/** Emits the dump immediately, in addition to the periodic one. */
void pbx_stats_dump(void);

#endif /* PBX_STATS_H */
