/*
 * pbx_stats.c — cumulative counters and their periodic log dump.
 *
 * See pbx_stats.h for why these are running totals rather than events.
 *
 * The line format is a contract with the host-side parser
 * (meshtastic-testbed-platform: src/pbx/collector/pbx_logd.py), so treat it as
 * one:
 *
 *   EVT stats v=1 <key>=<value> ...
 *
 *   * `EVT` is a stable prefix. The parser matches on it and ignores everything
 *     else on the console, so prose warnings can be reworded freely without
 *     breaking anything — which is the whole point of having this line.
 *   * `v=1` versions the field set. ADD keys freely; a parser that does not know
 *     a key ignores it. RENAMING or REMOVING one is a breaking change and must
 *     bump v, or the host silently reads zeros for a counter it thinks it has.
 *   * Values are unsigned and monotonic within a boot.
 */

#include "pbx_stats.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pbx_stats, LOG_LEVEL_INF);

/* atomic_t so the hot paths can increment without taking a lock: these are
 * touched from the BLE stack, the UART callback and the system work queue. */
static atomic_t s_counters[PBX_STAT_COUNT];

static struct k_work_delayable s_dump_work;

/* Keys are short because the whole set shares one log line, and Zephyr truncates
 * a message that exceeds CONFIG_LOG_BUFFER_SIZE — a truncated dump would lose
 * whichever counters sort last. Keep them terse and keep this table in the same
 * order as enum pbx_stat. */
static const char *const s_keys[PBX_STAT_COUNT] = {
	[PBX_STAT_TX_FWD]          = "tx_fwd",
	[PBX_STAT_TX_DROP_TXQ]     = "tx_drop_txq",
	[PBX_STAT_TX_DROP_ERR]     = "tx_drop_err",
	[PBX_STAT_TX_DECODE_FAIL]  = "tx_dec_fail",

	[PBX_STAT_RX_FRAMES]       = "rx_frames",
	[PBX_STAT_RX_OVERRUN_B]    = "rx_ovr_b",
	[PBX_STAT_RX_RESYNC]       = "rx_resync",
	[PBX_STAT_RX_DECODE_FAIL]  = "rx_dec_fail",

	[PBX_STAT_ROUTED]          = "routed",
	[PBX_STAT_BROADCAST]       = "bcast",
	[PBX_STAT_BCAST_FALLBACK]  = "bcast_fb",
	[PBX_STAT_PHONE_Q_DROP]    = "phone_q_drop",

	[PBX_STAT_BLE_CONNECT]     = "ble_conn",
	[PBX_STAT_BLE_DISCONNECT]  = "ble_disc",
	[PBX_STAT_BLE_REJECT]      = "ble_reject",
	[PBX_STAT_NODE_REG]        = "node_reg",

	[PBX_STAT_NODE_REBOOT]     = "node_reboot",
	[PBX_STAT_SESSION_REFETCH] = "sess_refetch",
	[PBX_STAT_KEEPALIVE_FAIL]  = "ka_fail",
};

BUILD_ASSERT(ARRAY_SIZE(s_keys) == PBX_STAT_COUNT,
	     "every pbx_stat needs a key, or the dump silently omits it");

void pbx_stats_inc(enum pbx_stat stat)
{
	if ((unsigned)stat < PBX_STAT_COUNT) {
		atomic_inc(&s_counters[stat]);
	}
}

void pbx_stats_add(enum pbx_stat stat, uint32_t delta)
{
	if ((unsigned)stat < PBX_STAT_COUNT) {
		atomic_add(&s_counters[stat], (atomic_val_t)delta);
	}
}

void pbx_stats_dump(void)
{
	/* Built into one buffer rather than logged per counter: the host must see
	 * a consistent snapshot, and nineteen separate lines could interleave with
	 * other modules' output or be partially dropped.
	 *
	 * Sized for the worst case, which is every counter at its widest decimal
	 * form: the key names total 181 bytes, and each of the 19 entries adds a
	 * space, an '=' and up to 10 digits, so 181 + 19*12 = 409 plus the prefix.
	 * 512 leaves room for keys added later. The runtime guard below is what
	 * actually protects correctness — this figure only keeps it from firing.
	 *
	 * The line is passed to LOG_INF as a %s argument, which Zephyr copies into
	 * the log buffer in deferred mode. CONFIG_LOG_BUFFER_SIZE has to be
	 * comfortably larger than this or dumps are what gets dropped first. */
	char line[512];
	size_t used = 0;
	int n;

	for (int i = 0; i < PBX_STAT_COUNT; i++) {
		n = snprintk(line + used, sizeof(line) - used, " %s=%u",
			     s_keys[i], (unsigned)atomic_get(&s_counters[i]));
		if (n < 0 || (size_t)n >= sizeof(line) - used) {
			/* Refuse to emit a half-line: a truncated dump reads as
			 * "these counters are zero" to the parser, which is worse
			 * than no dump at all. */
			LOG_ERR("EVT stats truncated at %d/%d keys — raise the buffer",
				i, PBX_STAT_COUNT);
			return;
		}
		used += (size_t)n;
	}

	LOG_INF("EVT stats v=1%s", line);
}

static void dump_work_handler(struct k_work *work)
{
	pbx_stats_dump();
	k_work_reschedule(k_work_delayable_from_work(work),
			  K_SECONDS(CONFIG_MESHTASTIC_STATS_PERIOD_S));
}

void pbx_stats_start(void)
{
	k_work_init_delayable(&s_dump_work, dump_work_handler);
	/* Dump immediately so a run has a baseline from its first second, rather
	 * than a first data point one period in with unknown history behind it. */
	k_work_schedule(&s_dump_work, K_NO_WAIT);
	LOG_INF("stats dump every %d s", CONFIG_MESHTASTIC_STATS_PERIOD_S);
}
