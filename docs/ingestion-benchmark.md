# Complete-message to book-update benchmark

Build and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/e2e_latency_benchmark \
  --input data/session-20260908T035554Z-11363.jsonl \
  --trials 5 --warmup-trials 1 --output benchmark-results.json
ctest --test-dir build --output-on-failure
```

`bash benchmark.sh [input.jsonl] [new-report.json] [off|on]` runs the same
build, five measured trials, one warm-up, and tests. Its default input remains
the recording above; its default output has a unique timestamp and process ID.

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
order statistics remain integer nanoseconds. Throughput divides record/level counts by the sum of individual handler durations
(`handler_elapsed_ns`). These durations include status publication and handler
cleanup, but exclude dataset string copies, wall-clock capture, connection resets,
result bookkeeping/serialization, writer drain, final hashing, and verification.
This is handler throughput under recorder contention, not wall-clock replay throughput.
Per-message latency still ends at book installation, before status publication.

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

Incremental envelopes stage only touched prices, with zero-quantity tombstones,
then update/erase existing nodes or transfer new nodes without allocating during
commit. Validation or staging allocation failure leaves the existing book intact.
Repeated prices retain envelope order. Snapshot-containing envelopes retain full
transactional staging. There are no per-update full-book hashes or invariant scans.
Runtime status contains BBO, level counts, sequence, state, counters, and progress,
with no level-map copies.

Each trial includes `records` with source `record_index`, `payload_bytes`,
`number_of_changes`, `snapshot_or_update`, `outcome`, and nullable `latency_ns`.
Change counts sum canonical levels across the complete envelope, including deletes
and repeated prices. Failed parsing has no canonical levels and reports zero;
non-L2/empty/unparseable envelopes are `unsampled`. Parsed duplicate/rejected L2
records retain their snapshot/update classification but have no successful latency.
`update_size_latency` reports nearest-rank statistics for 0, 1, 2–5, 6–20,
21–100, and 101+ changes. The last bucket starts at 101 to avoid overlap.
Snapshots, including mixed envelopes, are excluded from update buckets.
`levels_processed` and `levels_per_second` count levels in successful envelopes,
including snapshots. `nanoseconds_per_changed_level` is the sum of successful
book latencies divided by those levels; it is not an average of per-record ratios.
Largest payload covers all input records; largest change count covers parsed
canonical levels. Maximum-latency index covers successful samples, with the first
record winning ties (null if there are no samples).

Add `--profile-stages on` to enable per-record `stages` and aggregate
`stage_latency` statistics for queue enqueue, JSON decoding, schema validation,
fixed-point conversion, canonical construction, sequence validation, book
application, and status publication. Default is off, with no additional stage
clock reads. Aggregates include every input record; unreached stages are zero.
Schema timing includes timestamp/product validation and normalization bookkeeping,
minus nested conversion/construction durations. JSON timing also includes the
heartbeat parser's decoding and validation for heartbeat messages. Envelope
allocation, DOM destruction, and other uninstrumented glue mean these stages are
not an exhaustive decomposition of latency. Exceptions escaping the handler's
processing call can lose partial stage timings. Timers perturb small operations:
compare on/off runs before interpreting profiles or enabling production profiling.

The parser reserves canonical vectors from JSON counts, borrows DOM strings only
within their lifetime, and skips its owned raw copy when the handler already owns
an immutable envelope. L2 messages bypass the heartbeat decoder. The standalone
parser retains owned raw payloads by default. Decimal parsing already uses checked
character processing; product scales are resolved before the per-level loop.
The WebSocket buffer is reused, payload ownership is moved, and the recorder queue
shares `const RawEnvelope` objects. The existing three-thread runtime keeps all
serialization, disk writes, and per-record flushes on the writer thread. Queue
message/byte bounds and terminal recorder-failure behavior remain enforced.

The DOM parser still allocates per message; reusable parser scratch storage would
need a parser/API change. No custom allocator was introduced, and allocation
counts have not been profiled. Measure allocations before considering one.

For macOS Instruments, with full Xcode selected, build Release with symbols and run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g"
cmake --build build --parallel
xcrun xctrace record --template 'Time Profiler' --output /tmp/ingestion.trace \
  --launch -- ./build/e2e_latency_benchmark \
  --input data/session-20260908T035554Z-11363.jsonl \
  --trials 5 --warmup-trials 1 --output /tmp/instruments-report.json
```

Inspect the market thread's handler stacks separately from preload, writer, and
final verification stacks. Use Instruments Allocations for allocation counts.
In this environment `xcrun --find xctrace` failed: Instruments was not available,
so no Time Profiler or Allocations trace was collected.
