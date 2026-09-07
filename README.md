# Cross-Exchange Arbitrage Engine

A C++20 project building the market data foundation for a cross-exchange
arbitrage engine.

The current implementation includes exact fixed-point numeric types, canonical
market events, an order book, an offline Coinbase Level 2 parser, and raw message
recording and reading. Live exchange connections and trading logic are not yet
implemented.

## Documentation

- [Development setup](docs/development.md) — requirements, building, testing,
  and dependencies.
- [Architecture](docs/architecture.md) — components and their responsibilities.
- [Fixed-point numbers](docs/fixed-point.md) — numerical design and validation rules.
- [Coinbase adapter](docs/coinbase-adapter.md) — parsing behavior and product metadata.
- [Raw data format](docs/raw-data-format.md) — recording schema and storage guarantees.
