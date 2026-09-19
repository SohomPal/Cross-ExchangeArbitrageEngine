# Session manifests and deterministic replay

New captures use a dedicated, new session directory:

```sh
./build/cross-exchange-arbitrage capture --session data/sessions/session-example
# Stop with Ctrl-C; wait for the recorder to drain and the process to exit.
./build/cross-exchange-arbitrage replay --session data/sessions/session-example \
  --output data/replays/replay-example/result.json
```

Capture writes `manifest.inprogress.json` before starting the recorder. After reads
stop and the writer drains, flushes, closes and joins, it records file bytes,
SHA-256, record count, index bounds, runtime progress, configuration and build
metadata in `manifest.json`. Recording, queue and processing failures produce a
visibly incomplete final manifest. A crash leaves the in-progress document.
Existing directories and output files are rejected. Publication uses a closed,
fsynced temporary file and an atomic no-replace rename; readers cannot observe a partially written final document.
This provides atomic publication, not a guarantee against filesystem/device loss.

The raw log is `raw/coinbase.jsonl`. Legacy `--record FILE` and `--replay FILE`
remain supported, but standalone files lack session verification.

Replay checks manifest version, status, configuration, contained file path
(including symlinks), file size, SHA-256, count and contiguous zero-based record
indexes before calling the processor. A missing final newline is truncation.
`--allow-incomplete` permits diagnostic replay of an incomplete source; recorded
checksums and sizes are still enforced if present. Its result always has
`complete_source: false` and `research_valid: false`. An in-progress source with no
final checksums cannot prove capture completeness. Diagnostic replay processes
only the persisted prefix; lifecycle events beyond that prefix are omitted and
a finalized failed source ends INVALID. Stop capture before replaying.

Coinbase live and session replay use `CoinbaseMessageProcessor`, which owns no recorder.
Kraken uses `KrakenBookProcessor` for both paths, including mandatory CRC32 validation.
Replay supplies stored timestamps through `ReplayClock`; stage profiling is off.
A changed connection ID disconnects the old book, resets sequence and health,
and requires a fresh snapshot. Premature updates cannot restore validity.
Message-triggered integrity transitions are stored with their record index.
New manifests also store `lifecycle_events`: state, reason and
`before_record_index`, including health timeouts, recovery and shutdown between
messages. Replay applies those events before the corresponding record; an index
equal to the record count applies after the last message. This reproduces the
terminal disconnected state of a clean Coinbase shutdown. Kraken preserves its final
validated book on normal shutdown and records its hash in the manifest. Legacy JSONL lacks these
events; its connection changes are inferred from received envelopes.

Version 1 captures support Coinbase BTC-USD and Kraken BTC_USD, with processor
version 1. Both use price scale 2 and quantity scale 8 by default. Coinbase replay
supports recorded scales from 0 through 18; Kraken validates its fixed scales,
BTC/USD venue mapping, and subscribed depth. Unsupported configurations and mismatched configuration hashes fail closed. Replay
constructs the symbol mapper from the validated recorded precision.

Canonical hashes use UTF-8 compact JSON, lexicographically ordered object keys,
integer numbers, explicit nulls, and no insignificant whitespace. Bids are ordered
by descending price and asks by ascending price; levels are `[price, quantity]`.
The book hash covers both complete level arrays, book state and book sequence. The transition hash covers the
ordered transition array. The result hash covers functional result fields plus
the full final book, excluding source session ID, manifest hash, and the result
hash itself. Replay execution duration, throughput, output path and current clock
do not enter it. Kraken state-time metrics derive only from recorded timestamps.
The output includes all outcome counts, best prices, sequence, level counts,
transitions and hashes. `--include-final-book` additionally emits the levels;
it does not change the deterministic result hash.

Benchmark reports also omit repeated full books by default. Pass
`--include-final-book` to `e2e_latency_benchmark` to retain them for diagnostics.
Historical benchmark artifacts retain their original schema and measurements.

Capture accepts `--queue-messages`, `--queue-bytes`,
`--minimum-free-disk-bytes`, and the development fault option
`--force-disconnect-after-seconds`. These operational limits are not parser
configuration and do not enter `configuration_hash`; their effects are captured
in status, persisted records and lifecycle events.

## Validation

Local Release validation on 2026-09-13 passed all 14 CTest targets with
`-Wall -Wextra -Wpedantic -Werror`, plus the CI clang-format checks. The session
suite covers publication timing and overwrite refusal, repeated hashes, corrupt
sources, incomplete diagnostics, record ordering, reconnect snapshot gating,
symlink containment, and live-manager/replay agreement through recovery and stop.
A CLI disk-preflight failure also produced `recording_failure` as expected.
No new live exchange capture or performance measurement was run for this change;

Kraken capture and checksum-validated replay are documented in [kraken-adapter.md](kraken-adapter.md).
