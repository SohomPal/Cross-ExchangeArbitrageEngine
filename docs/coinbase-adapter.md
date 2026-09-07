# Coinbase Level 2 Adapter

The `coinbase_adapter` library parses synthetic Advanced Trade Level 2 fixtures
without a network connection. `adapters::coinbase::CoinbaseL2Parser::parse` returns
`Parsed`, `Ignored` for other channels, or `Error` with a diagnostic and no events.
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
