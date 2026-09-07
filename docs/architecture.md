# Architecture

The project currently provides offline market data components. Live exchange
connections, feed session management, and trading logic are not implemented.
The executable is a fixed-point parsing demonstration; it does not yet wire
the components into a running engine.

## Components

| Component | Responsibility | Location |
| --- | --- | --- |
| `arbitrage_core` | Fixed-point values, canonical market events, and order book state | `include/core/`, `src/core/` |
| `coinbase_adapter` | Translate Coinbase Level 2 JSON into canonical events using product metadata | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `raw_recording` | Write and read raw messages with receive timestamps and connection metadata | `include/recording/`, `src/recording/` |
| `arbitrage_engine` | Demonstrate fixed-point parsing | `src/main.cpp` |

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
intended ingestion order is to record a message before parsing it; a future
feed session must connect these steps and handle sequence gaps and reconnection.

## Detailed documentation

- [Fixed-point numbers](fixed-point.md): numeric types, exact parsing, scales,
  and increment validation.
- [Coinbase adapter](coinbase-adapter.md): envelope validation, timestamps,
  product mapping, and parser outcomes.
- [Raw data format](raw-data-format.md): record schema, payload preservation,
  corruption handling, and flush policy.
- [Development setup](development.md): build commands, dependencies, and CI.
