# Complete-message to book-update baseline

Build and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/e2e_latency_benchmark \
  --input data/session-20260908T035554Z-11363.jsonl \
  --trials 5 --warmup-trials 1 --output benchmark-results.json
ctest --test-dir build --output-on-failure
```

The output must be a new file. Input is format-version-1 raw JSONL. Input bytes
are hashed with SHA-256 and messages are loaded into memory before any trial.
At least one warm-up is mandatory. Release builds are mandatory; timing
thresholds are deliberately absent from CI.

The live Beast read callback captures `steady_clock` immediately after a complete
WebSocket message is delivered, before converting Beast's buffer to an owned
string. Fragmented messages are measured once, after complete reassembly. The
benchmark starts with an owned string copied from the preloaded dataset, then
captures a fresh monotonic receive timestamp. It does not reuse historical
receive timestamps or measure network transit, individual bytes, or exchange time.
Both modes call `CoinbaseMessageHandler::handle` on their market-data thread.

The timed interval includes immutable envelope construction, bounded FIFO enqueue,
JSON parsing, canonical event creation, session sequence validation, and the
transactional application of the complete envelope. It ends after levels,
book sequence, and book state have been installed. BBO accessors reflect that
installed state. Runtime status is published afterward, outside the sample.
Disk serialization, writes, and flushes run concurrently on the recording thread.
The measurement includes their scheduling contention, but does not wait for
recording acknowledgement. Beast buffer conversion is specific to the live path.

Each successfully applied envelope contributes exactly one integer-nanosecond
sample. Snapshot-containing envelopes, including mixed snapshot/update envelopes,
are classified as snapshots; envelopes containing only updates are incremental
updates. Counts named `snapshots` and `updates` count sampled envelopes, not
canonical events. The combined category is also reported. Heartbeats,
acknowledgements, unknown channels, duplicates, out-of-order sequences, parse
errors, sequence gaps, and rejected updates have no successful latency sample.
An empty L2 envelope does not produce a sample.

For each category the report includes sample count, minimum, arithmetic mean,
p50, p95, p99, and maximum in nanoseconds. Percentiles use nearest rank:
`sorted[ceil(percentile * N) - 1]`. Empty categories have zero-valued statistics
and zero samples. Mean uses a wide floating accumulator; individual samples and
order statistics remain integer nanoseconds. Throughput covers the market-data
loop, including dataset string copies and status publication, and excludes writer
drain, final book hashing, and output verification.

Every trial uses a fresh parser, sequence tracker, book, health state, queue,
and writer file in a unique temporary directory. Main starts and joins exactly
two workers. The market worker processes preloaded input; the recorder worker
uses the same writer function as live mode. Input connection-ID changes reset
sequence/health and require a new snapshot. The benchmark processes every record,
including diagnostic failures; it does not synthesize network reconnections.

After closing the queue and draining/joining the writer, the trial checks every
written index, connection ID, venue, and payload by rereading its output. Book
contents are captured on the market thread after drain. Canonical book contents
are ordered bids and asks (integer ticks and atoms), book sequence, and state.
Their compact JSON, with lexicographically sorted object keys, is SHA-256 hashed.
The report retains both complete contents and hash, plus the session sequence
(which may advance on non-L2 messages), final BBO, counts, and all four progress
indexes. All deterministic values must agree across warm-up and measured trials.
The CI fixture additionally checks explicit golden contents, hash, and counts.
Temporary raw trial files are removed afterward.

Queue defaults match live mode: 4,096 messages and 64 MiB of payload bytes.
`--queue-messages` and `--queue-bytes` explicitly override these bounds in either
mode and benchmark reports record them. No pacing or automatic queue growth is
added. A dataset that overruns the chosen bounds fails the benchmark. Keep the
same dataset and queue configuration for comparisons.

This baseline deliberately preserves existing full-book transactional copying
and per-record flushing. Lock-free queues, batching, acknowledgement-before-apply,
and further parser/book optimizations remain deferred.
