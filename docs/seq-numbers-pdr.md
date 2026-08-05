# Per-pair sequence numbers — design proposal

**Status:** proposal — not yet implemented. No source has been modified.
**Scope:** `src/proxy_protocol.{h,c}` (wire format v2), plus the senders/receivers
that build and parse the header (`src/router.c`, `src/main.c`, the phone app).
**Goal:** make packet delivery ratio (PDR) and loss-burst structure *directly
measurable* from the received stream, with no external bookkeeping.

## Problem

The proxy header today is 9 bytes and carries no notion of ordering:

```
offset  field      size
  0     VERSION    1     = 0x01
  1     SRC_ID     4     sender proxy_id
  5     DST_ID     4     receiver proxy_id
  9     content    N
```

(`src/proxy_protocol.h:15` for the layout, `src/proxy_protocol.c:11` for the
parser.)

A receiver therefore cannot tell the difference between *"the sender sent 40
messages and I got 40"* and *"the sender sent 60 and I got 40"*. To measure loss
today you have to correlate two logs — the sender's and the receiver's — by
timestamp and content, which is fragile over a LoRa mesh where latency is
seconds, hops reorder, and Meshtastic's own retransmissions duplicate packets.
And even when the count works out, the *shape* of the loss (isolated drops vs.
long outage bursts) is invisible, which is the part that actually matters for a
messaging UX.

## Design

**Add a 2-byte monotonic sequence number, per `(src, dst)` pair, immediately
after the 9-byte header.**

```
offset  field      size
  0     VERSION    1     = 0x02        <- bumped
  1     SRC_ID     4     sender proxy_id
  5     DST_ID     4     receiver proxy_id
  9     SEQ        2     little-endian uint16, monotonic per (SRC_ID, DST_ID)
 11     content    N
```

Header grows 9 → 11 bytes; content max drops by 2 (`PROXY_CONTENT_MAX`
recomputes from `PROXY_HEADER_SIZE`, so the constant is the only edit).

The counter is **per pair, not global**: each sender keeps one `uint16` per
destination it talks to, incremented once per transmitted message and wrapping
modulo 2^16. Per-pair (rather than per-sender) is what makes the receiver's
arithmetic trivial — every stream it observes is a single dense range with no
holes that belong to somebody else.

### What it buys

**1. PDR from one side's log.** The receiver counts distinct sequence numbers it
saw and compares against the span the sender must have emitted:

```
pdr = unique_received / (seq_max - seq_min + 1)
```

`unique_received` — not raw packet count — because the mesh delivers duplicates;
deduplicating by `seq` is what makes the numerator correct, and it comes for
free.

**2. Loss-run distribution.** The missing sequence numbers group into
consecutive runs. The histogram of run lengths distinguishes the two failure
modes that a single PDR number conflates:

- many runs of length 1 → independent per-packet loss (SNR margin, collisions)
- few long runs → outages (node down, phone out of range, a hop lost)

A 90 % PDR made of ten isolated drops and a 90 % PDR made of one 60-second hole
are very different products.

**3. Reorder and duplicate counts**, as a byproduct: any arrival with
`seq < last_seen_seq` is a reorder; any repeat of a seen `seq` is a duplicate.

### Sender state

One small table per sender, keyed by destination:

```c
struct seq_entry {
    proxy_id_t dst;        /* key; all-zero => empty slot */
    uint16_t   next_seq;   /* next value to stamp         */
};
```

The proxy itself only needs this if it originates proxy-protocol traffic (it
currently forwards phone-built frames verbatim, `docs/client-integration.md`
§4.2). For the router model the counter lives in the **phone app**, which is
already the party building the header.

### Receiver state

Per `(src, dst)` pair: `seq_min`, `seq_max`, and a seen-set. For a bounded
implementation, a sliding bitmap (e.g. 1024 bits = 128 bytes, covering the last
1024 sequence numbers) is enough — anything older than the window is already
past any plausible reorder or retransmit horizon. For offline analysis in
`tools/`, a plain Python `set` is simpler and has no window at all.

### Code sketch

```c
/* proxy_protocol.h */
#define PROXY_VERSION      0x02U
#define PROXY_SEQ_SIZE     2U
#define PROXY_HEADER_SIZE  (1U + PROXY_ID_SIZE + PROXY_ID_SIZE + PROXY_SEQ_SIZE)  /* 11 */
#define PROXY_OFF_SEQ      (PROXY_OFF_DST + PROXY_ID_SIZE)   /* 9  */
#define PROXY_OFF_CONTENT  PROXY_HEADER_SIZE                 /* 11 */

struct proxy_header {
    proxy_id_t     src;
    proxy_id_t     dst;
    uint16_t       seq;          /* NEW */
    const uint8_t *content;
    uint16_t       content_len;
};
```

`proxy_header_parse()` reads the two bytes little-endian:

```c
out->seq = (uint16_t)payload[PROXY_OFF_SEQ] |
           ((uint16_t)payload[PROXY_OFF_SEQ + 1] << 8);
```

`proxy_header_build()` gains a `uint16_t seq` parameter — a signature change, so
every call site is flagged by the compiler rather than silently building a
short frame. `proxy_header_to_str()` extends to
`"[v02][src=…][dst=…][seq=…][content=N B]"`, which makes the proxy's own log
line a usable PDR trace on its own.

### Analysis (tools side)

```python
def pdr(seqs):                      # seqs: list of received seq values, one pair
    uniq = set(seqs)
    span = max(uniq) - min(uniq) + 1
    return len(uniq) / span, span - len(uniq)

def loss_runs(seqs):                # -> {run_length: count}
    uniq = sorted(set(seqs))
    runs, hist = [], {}
    for a, b in zip(uniq, uniq[1:]):
        if b - a > 1:
            runs.append(b - a - 1)
    for r in runs:
        hist[r] = hist.get(r, 0) + 1
    return hist
```

Both assume a wrap-free window; see below.

## Edge cases

- **Wrap.** 2 bytes = 65 536 messages per pair. At LoRa messaging rates that is
  effectively unreachable in one session, but the arithmetic must not silently
  break: unwrap by tracking an epoch counter that increments when a new `seq` is
  far *below* the previous one (`prev - seq > 32768`), then compute on the
  unwrapped value. A `uint32` would sidestep this entirely at the cost of 2 more
  bytes of every payload — not worth it at 141–191 bytes of content budget.
- **Sender restart.** A phone that restarts and resets `next_seq` to 0 looks like
  a giant backward jump. Persist the counter alongside the `proxy_id`
  (`docs/reconnect-persistence.md` proposes exactly this kind of registry), or
  treat a backward jump beyond the reorder window as "new session" and start a
  fresh measurement interval rather than reporting 0.001 PDR.
- **`seq_min` bias.** PDR is measured over the *observed* span, so packets lost
  before the first received one and after the last are invisible. For a
  controlled test, have the sender emit a known count and compare against it;
  for field data, report the span alongside the ratio so the denominator is
  auditable.
- **Broadcast fallback.** When `DST_ID` is unregistered the proxy broadcasts to
  every phone (`docs/client-integration.md` §4.3). Receivers must key their
  counters by `(SRC_ID, DST_ID)` from the header — not by "packets I received" —
  or a phone will mix another pair's stream into its own denominator.
- **Version coexistence.** The parser currently rejects any version that is not
  `PROXY_VERSION` (`src/proxy_protocol.c:17`), so a v1 sender and a v2 receiver
  simply do not talk. Either flag-day both ends, or accept both versions and set
  `seq = 0` / a `has_seq` flag for v1 frames. Given both ends are ours and the
  format is pre-1.0, a flag day is the cheaper option.

## Cost

- **+2 bytes per message** (~1–1.4 % of the payload budget), no extra airtime
  beyond that.
- **Sender:** 6 bytes of RAM per destination.
- **Receiver:** 128 bytes per pair for a 1024-entry bitmap, or unbounded in
  offline tooling.
- **No protocol round-trips** — the measurement is entirely passive, which is
  the point: it costs nothing when nobody is measuring.
