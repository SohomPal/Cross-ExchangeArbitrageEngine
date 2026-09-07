# Cross-Exchange Arbitrage Engine

A C++20 project building the market data foundation for a cross-exchange
arbitrage engine.

The current implementation includes exact fixed-point numeric types, canonical
market events, an order book, a live Coinbase Level 2 feed, and raw message recording with offline replay.
Trading logic is not implemented.

```sh
./build/arbitrage_engine --record data/session.jsonl
# Stop with Ctrl-C, then replay the recording:
./build/arbitrage_engine --replay data/session.jsonl
```

The recording path must be new. The feed subscribes to public BTC-USD `level2`
and `heartbeats`, records every message before parsing, and prints book state
and BBO once per second. SIGINT/SIGTERM closes the socket and flushes the file.
Errors invalidate the book and exit nonzero. This slice uses one connection and
one thread; reconnection, gap recovery, and stale-book detection are deferred.

## Documentation

- [Development setup](docs/development.md) — requirements, building, testing,
  and dependencies.
- [Architecture](docs/architecture.md) — components and their responsibilities.
- [Fixed-point numbers](docs/fixed-point.md) — numerical design and validation rules.
- [Coinbase adapter](docs/coinbase-adapter.md) — parsing behavior and product metadata.
- [Raw data format](docs/raw-data-format.md) — recording schema and storage guarantees.
