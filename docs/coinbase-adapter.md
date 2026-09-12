# Coinbase Level 2 Adapter

The `coinbase_adapter` library parses Advanced Trade Level 2 messages independently
of networking. `adapters::coinbase::CoinbaseL2Parser::parse` returns
`Parsed`, `Ignored` for other channels or unsupported message types, or `Error`
with a diagnostic and no events. Explicit Coinbase errors are errors even without
a channel. A message must have a channel or type; malformed JSON remains an error.
An entire envelope is validated before any canonical events are returned. The
result owns `raw_message`, preserving individual change timestamps and metadata.
The envelope timestamp becomes `exchange_time`; receive timestamps are supplied
by the caller. UTC timestamps accept zero or 1–9 fractional digits and reject
invalid dates, leap seconds, offsets, and values outside signed 64-bit nanoseconds.

The default `CoinbaseSymbolMapper` uses synthetic BTC-USD scales of 2 and 8.
Inject a mapper built from `CoinbaseSymbolMapper::Products` to supply product
metadata; defaults are not a live product precision guarantee. Price and quantity
fields must be decimal strings and go directly to the exact core parsers.
Each canonical event carries the envelope sequence; sequence gap detection and
reconnection are enforced by the connection manager (once per envelope). `OrderBook` remains
independent of Coinbase JSON. Empty event/update arrays are accepted as empty
batches. Individual `event_time` fields are required and validated.

Schema references: [Coinbase Level 2](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/websocket/level2)
and [WebSocket overview](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview).


## Live session

`coinbase_websocket` uses Asio, Beast, and OpenSSL to connect to
`wss://advanced-trade-ws.coinbase.com` with SNI, certificate verification, hostname
verification, and WebSocket timeouts. It sends separate public `level2` (BTC-USD)
and `heartbeats` subscriptions sequentially, with no credentials. The shared
message handler enqueues every delivered payload before parsing, including
heartbeats, acknowledgements, duplicates, and failures. A separate recording
thread writes and flushes accepted messages. One recording
contains all successful WebSocket sessions, with monotonically increasing connection
IDs. Failed handshakes consume attempts but do not consume connection IDs.

Only Valid books expose best prices or a two-sided market. Levels remain available
for diagnostics in other states. Sequence gaps, malformed L2, unsupported products,
explicit errors, recorder failures, and update-before-snapshot invalidate immediately.
Queue rejection and recorder failures are terminal and never reconnect.
Transport errors disconnect; freshness failures mark Stale. For recoverable feed
and network failures, exactly one retry is
scheduled, and the next attempt enters Resyncing then Initializing after subscriptions
are sent. A fresh, transactionally installed snapshot is required to become Valid.
A new client and callback generation prevent abandoned sessions from changing state.

Sequence continuity spans every envelope carrying `sequence_num`, including L2,
heartbeats, subscriptions, and unknown channels. Duplicates and old sequences are
ignored and counted; a gap on any channel triggers recovery before application.
Missing sequences on L2 and malformed sequences on any channel are errors.
Unsequenced non-L2 control messages are allowed and do not advance the tracker.
Only accepted heartbeats refresh heartbeat health; they never modify book levels.
The connection sequence can start before the snapshot, but L2 updates and empty L2
envelopes cannot establish the book. Each reconnect resets both requirements.
The status sequence describes the connection; OrderBook::last_sequence describes
the last applied L2 event. Heartbeats remain outside MarketEvent.

Health checks default to every second, with five-second message and heartbeat age
limits. Initial connection time provides the grace period when no heartbeat has arrived.
Instrument inactivity limits are optional and disabled by default. All durations use
monotonic time; unchanged best prices do not imply staleness. These are development
defaults configurable through FeedHealthConfig, not exchange guarantees.

Retries use deterministic 250 ms, 500 ms, 1 s, 2 s, 4 s, 8 s, then 10 s delays.
Only a fresh snapshot resets backoff. Shutdown cancels retries and sockets.
Metrics expose session counts, reconstruction timings, integrity counters, freshness,
and cumulative valid/stale/invalid durations (updated at health checks and transitions).
The copied status line includes runtime/connection/book state, sequence, BBO,
queue size/bytes, all four progress positions and counts, and any fatal recording
error or unpersisted processed tail. Recovery metrics remain owned by the market
thread.

Run `arbitrage_engine --record NEW_FILE.jsonl`, then stop with SIGINT or SIGTERM.
Replay accepts increasing connection IDs and resets sequence/book usability at each
boundary. After an integrity failure it ignores the rest of that connection and may
resume at the next boundary. Raw recordings contain messages, not transport/timer
events, so replay reconstructs books but cannot reproduce live timeout metrics.

Protocol references:
[Advanced Trade channels](https://docs.cdp.coinbase.com/coinbase-business/advanced-trade-apis/websocket/websocket-channels)
and [sequence guidance](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview).

## Shared sequence example

A captured connection contained L2 sequence 4, subscription acknowledgements 5 and
6, heartbeat 7, then L2 8. All five envelopes participate in continuity checking;
only the L2 envelopes modify the book. Tracking L2 alone incorrectly identified
this complete stream as a gap.
