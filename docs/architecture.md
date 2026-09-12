# Architecture

Live capture uses exactly three threads:

| Thread | Ownership |
| --- | --- |
| Main/control | Configuration, worker startup/join, shutdown requests, once-per-second copied status output, exit status |
| Market data | Asio context, TLS/WebSocket, receive clocks, shared message handler, sequence/health, reconnection, order book, latency |
| Raw recorder | Output stream, serialization, writes, flush/close, written position |

There is one mutex/condition-variable FIFO from market data to recording, bounded
by both 4,096 messages and 64 MiB of payload bytes. An immutable shared envelope
owns the moved payload. The market thread enqueues before parsing and publishes
a small mutex-protected status snapshot after processing. Main never accesses
live book containers. No book locks are needed. Offline replay remains a
single-thread diagnostic command. Trading is deferred.

DNS resolution runs synchronously on the market thread to avoid Asio's hidden
resolver worker. Consequently, a slow OS resolver can delay market-thread
shutdown until resolution returns. Socket operations and reconnect timers are
asynchronous on that same thread.

## Components

| Component | Responsibility | Location |
| --- | --- | --- |
| `arbitrage_core` | Fixed-point values, canonical market events, and order book state | `include/core/`, `src/core/` |
| `coinbase_adapter` | Translate Coinbase Level 2 JSON into canonical events using product metadata | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `raw_recording` | Write and read raw messages with receive timestamps and connection metadata | `include/recording/`, `src/recording/` |
| `coinbase_websocket` | TLS connection, sequential subscriptions, reads, and close | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `coinbase_recovery` | Sequence continuity, feed health, backoff, session generations, and snapshot gating | `include/adapters/coinbase/`, `src/adapters/coinbase/` |
| `pipeline_runtime` | Three-thread lifecycle, bounded raw FIFO, copied runtime status and writer failure delivery | `include/pipeline/`, `src/pipeline/` |
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
message handler enqueues before parsing and invalidates the book on recording,
parsing, or application failure. Network errors disconnect it. All non-Valid
states hide best prices while retaining diagnostic levels. Sequence gaps and
monotonic health timeouts trigger bounded retries with fresh session IDs.
Heartbeats are connection events outside the market-event variant. Transport failures are
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

## Capture failure and shutdown

Queue-full rejection prevents parsing, invalidates the book, closes the transport,
and exits nonzero with `RAW_RECORDING_QUEUE_FULL`. A closed queue is also terminal.
The recorder checks available disk space before reporting readiness; no network
connection starts before readiness. The default minimum is 64 MiB, configurable
with `--minimum-free-disk-bytes`. Normal write/flush failures are still checked.

Writer errors save a shared terminal state and post an invalidation/stop callback
onto the market-data Asio context. The writer never touches network or book state.
Terminal recording failures cancel reconnects; ordinary network/feed failures
retain existing recovery behavior. Callback registration is cleared under the
same mutex used by the writer before the context and callback owners are destroyed.

Shutdown stops accepting WebSocket messages, closes the queue, drains accepted
messages, flushes/closes output, joins both workers, and prints final counts.
An actual writer failure may prevent drain; it is reported and remains nonzero.
All started workers are joined, including startup/error paths. Existing
per-append and caller flushes are preserved; there is no batching or fsync change.

Status distinguishes last received, enqueued, successfully written, and processed
indexes. Processed means the handler finished examining an accepted envelope,
including ignored/rejected market messages. Clean shutdown makes all four agree.
Enqueued/processed does not mean persisted. A failed writer reports any processed
but unwritten tail as `unpersisted_processed_range=FIRST-LAST`, including the
case where no record was successfully written. “Written” means the existing
stream-write/flush contract, not a new power-loss durability guarantee.

See [the latency benchmark](ingestion-benchmark.md) for timing boundaries,
repeatability checks, and [measured baseline results](ingestion-benchmark-results.md).
