# Architecture

The executable composes a single-thread Coinbase feed: WebSocket → raw recording
→ canonical parsing → L2 order book → periodic health/BBO display. Offline replay
uses the same message pipeline. Trading and automatic feed recovery are deferred.

## Components

| Component | Responsibility | Location |
| --- | --- | --- |
| `arbitrage_core` | Fixed-point values, canonical market events, and order book state | `include/core/`, `src/core/` |
| `coinbase_adapter` | Translate Coinbase Level 2 JSON into canonical events using product metadata | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `raw_recording` | Write and read raw messages with receive timestamps and connection metadata | `include/recording/`, `src/recording/` |
| `coinbase_websocket` | TLS connection, sequential subscriptions, reads, and close | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `arbitrage_engine` | Compose live ingestion or offline replay | `src/main.cpp` |

Both the Coinbase adapter and raw recording library depend on the core library.
Exchange-specific JSON handling stays outside the core order book.

## Market data boundaries

Adapters emit `BookSnapshot` and `BookUpdate` events. Each event carries its
venue, instrument, exchange and receive timestamps, and optional sequence.
Normalized quantities are absolute: a positive quantity replaces a price level,
and zero deletes it. Adapters are responsible for converting venue-specific
semantics to this convention.

`OrderBook` maintains bid and ask levels for a venue and instrument, exposes
best prices and book state, and applies canonical snapshots and updates.
Rejected events preserve the existing levels, sequence, and state. Snapshots
reject duplicate side/price entries; updates process repeated entries in order.

Raw recording preserves messages for later debugging, replay, and audit. The
message pipeline records before parsing and invalidates the book on recording,
parsing, or application failure. Network errors also invalidate it. Invalidation
retains diagnostic levels; consumers must check state before using prices. Sequence
gaps, stale detection, and reconnection remain future work. Transport failures are
reported out of band and are not raw messages; replay reconstructs received data,
not terminal network health.

## Detailed documentation

- [Fixed-point numbers](fixed-point.md): numeric types, exact parsing, scales,
  and increment validation.
- [Coinbase adapter](coinbase-adapter.md): envelope validation, timestamps,
  product mapping, and parser outcomes.
- [Raw data format](raw-data-format.md): record schema, payload preservation,
  corruption handling, and flush policy.
- [Development setup](development.md): build commands, dependencies, and CI.
