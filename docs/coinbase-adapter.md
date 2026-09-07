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
reconnection are responsibilities of a future feed session. `OrderBook` remains
independent of Coinbase JSON. Empty event/update arrays are accepted as empty
batches. Individual `event_time` fields are required and validated.

Schema references: [Coinbase Level 2](https://docs.cdp.coinbase.com/api-reference/advanced-trade-api/websocket/level2)
and [WebSocket overview](https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/websocket/websocket-overview).


## Live session

`coinbase_websocket` uses Asio, Beast, and OpenSSL to connect to
`wss://advanced-trade-ws.coinbase.com` with SNI, certificate verification, hostname
verification, and WebSocket timeouts. It sends separate public `level2` (BTC-USD)
and `heartbeats` subscriptions sequentially, with no credentials. The shared
`CoinbaseMessagePipeline` records every delivered payload before parsing, including
heartbeats and acknowledgements. Errors stop processing and invalidate the book
without clearing its levels.

Run `arbitrage_engine --record NEW_FILE.jsonl`, stop with SIGINT or SIGTERM, then
run `arbitrage_engine --replay FILE.jsonl`. The one-second display includes state,
sequence, BBO, level counts, message count, and parse errors. Replay accepts a
single Coinbase session (connection id 1). Network failures terminate nonzero;
recovery, gap checks, and stale detection are deferred. The current BTC-USD
configuration uses price scale 2 and quantity scale 8 and rejects excess precision.
