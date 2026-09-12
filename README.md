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
and `heartbeats`, enqueues every raw message before parsing, and prints book state
and BBO once per second. SIGINT/SIGTERM closes the socket and flushes the file.
Feed integrity failures make the book unavailable and trigger bounded reconnection.
Queue rejection or recording failure stops capture with a nonzero exit status.
Each session requires a fresh snapshot. Heartbeat and no-message timeouts use
monotonic time. The process uses exactly three threads: main/control, market data (one I/O thread),
and raw recording. Recordings span connection IDs.
Use `--force-disconnect-after-seconds 30` for the development recovery exercise.
Sequence continuity includes all channels; only L2 messages modify the book.

## Documentation

- [Development setup](docs/development.md) — requirements, building, testing,
  and dependencies.
- [Architecture](docs/architecture.md) — components and their responsibilities.
- [Fixed-point numbers](docs/fixed-point.md) — numerical design and validation rules.
- [Coinbase adapter](docs/coinbase-adapter.md) — parsing behavior and product metadata.
- [Raw data format](docs/raw-data-format.md) — recording schema and storage guarantees.

Latency benchmarking and the three-thread live runtime are documented in
[ingestion-benchmark.md](docs/ingestion-benchmark.md) and
[architecture.md](docs/architecture.md).
