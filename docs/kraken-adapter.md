# Kraken L2 capture and replay

Kraken runs alone in the existing three-thread layout: main/status, market data,
and raw recorder. Both venues use `transport::WebSocketClient`; Kraken selects
`ws.kraken.com:443/v2` and a single book subscription. It does not subscribe to
heartbeats. Any incoming message refreshes connection liveness; the processor
also retains the latest heartbeat time. Five seconds without messages triggers
recovery, as does failing to obtain a valid snapshot within fifteen seconds.

```sh
./build/cross-exchange-arbitrage capture --venue kraken --instrument BTC_USD \
    --depth 100 --duration-seconds 300 --session data/sessions/kraken-smoke
./build/cross-exchange-arbitrage replay --session data/sessions/kraken-smoke \
    --output data/sessions/kraken-smoke/replay-1.json --include-final-book
./build/cross-exchange-arbitrage replay --session data/sessions/kraken-smoke \
    --output data/sessions/kraken-smoke/replay-2.json --include-final-book
```

Omit `--session` to generate a directory, or omit `--duration-seconds` to capture
until SIGINT/SIGTERM. Supported depths are 10, 25, 100, 500 and 1000. Recording
queue and minimum free disk options also apply. `--force-disconnect-after-seconds`
exercises reconnection and snapshot recovery.

Numeric tokens are protected before JSON DOM decoding. Prices and quantities
never pass through binary floating point. Original lexemes remain in parsed
changes and the Kraken checksum book; exponent notation expands using decimal
string operations. BTC/USD uses canonical BTC_USD with price scale 2 and quantity
scale 8. Excess precision is rejected rather than rounded. Kraken sequences are
always absent.

Every snapshot/update stages a separate checksum book, applies all changes in
order, truncates each side, and verifies the top-ten CRC32 before installing a
canonical book. A rejected message preserves installed levels and prevents BBO
publication. A subsequent update cannot restore validity; a matching snapshot
is required. Crossed books also fail closed.

The official checksum fixture is adapted from
[Kraken's v2 checksum guide](https://docs.kraken.com/exchange/guides/websockets/book-checksum-v2)
and must produce unsigned CRC32 `3310070434`. Its numbers are unquoted to test the
actual wire representation, including significant trailing zeros.

Sessions record `raw/kraken.jsonl`, subscription depth, canonical symbol mapping,
precision, recovery lifecycle events, and Kraken metrics. The `*_time` metrics
are nanoseconds accumulated between received messages, using recorded monotonic
timestamps; replay does not introduce wall-clock timing. Status prices are integer
price ticks (100 ticks per USD).

Replay uses the same parser and processor, including checksum validation and
snapshot recovery. Compare `final_book_hash`, `state_transition_hash`, and
`deterministic_result_hash` across runs. Raw file integrity and manifest validation
remain mandatory, with incomplete sessions requiring `--allow-incomplete`.

## Validation on 2026-09-17

A 310-second Release capture in `data/sessions/kraken-smoke-20260917` finalized
with status `complete`. It recorded 33,649 messages, validated 33,337 checksums,
and reported zero checksum, parsing, or subscription failures. A forced disconnect
at 15 seconds recovered with a fresh validated snapshot. Both final sides held
100 levels and the best bid was below the best ask. The capture received 307
heartbeats and recorded one reconnect and one recovery snapshot.

Two full replays agreed on every result field, including all metrics and state
transitions. Their final-book hash also matched the hash saved by live capture:

- Final book: `6d998205dcf5dea45dcb3f84b934eb4cbc7a63966c90fe92e54298a9dd5cfd9f`
- Deterministic result: `c72a6b804c2f7eca8d23c3a5b93bed3346e3540fcf15d6c7da6e508c5698270d`

Validation used `build-kraken` because the existing build directory initially
could not be written. The Release build uses `-Wall -Wextra -Wpedantic -Werror`;
formatting uses the CI-pinned clang-format 18.1.8.
