# Development Setup

## Requirements

- CMake 3.20 or newer.
- A C++20 compiler.

- Boost development headers and OpenSSL development libraries.

Install system dependencies with `brew install boost openssl@3` on macOS or
`sudo apt-get install libboost-dev libssl-dev` on Ubuntu. JSON and test libraries
are vendored. Once dependencies are installed, builds and tests need no internet.

## Build and test

Run from the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/arbitrage_engine --record data/session.jsonl
```

Stop with Ctrl-C and run `./build/arbitrage_engine --replay data/session.jsonl`
to reconstruct the final book offline. The live command requires network access;
it is never run in CI. Tests use fixtures and a local TLS WebSocket peer. The
committed localhost key is a public test-only key and must never be used elsewhere.
TLS verification remains enabled in tests and production.

## Dependencies

- Tests use [doctest v2.4.12](https://github.com/doctest/doctest/tree/v2.4.12),
  vendored in `tests/vendor/doctest.h` with its MIT license in the header.
- JSON parsing uses [nlohmann/json v3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0),
  vendored in `third_party/nlohmann/json.hpp` with its MIT license in the header.

## Continuous integration

GitHub Actions builds and tests on Linux and macOS with compiler warnings
treated as errors (`-Wall -Wextra -Wpedantic -Werror`). A separate formatting
check uses clang-format 18.1.8 for the Coinbase adapter headers, sources, and
parser tests. See [the workflow](../.github/workflows/ci.yml) for the exact checks.

## Recovery fault exercise

Use a new output file (the output parent directory must exist):

```sh
mkdir -p data/raw
./build/cross-exchange-arbitrage --venue coinbase --instrument BTC-USD \
  --output data/raw/coinbase-recovery-test \
  --force-disconnect-after-seconds 30
```

The one-shot development fault disconnects locally after 30 seconds. Observe Valid,
Disconnected/Resyncing, Initializing, then Valid with a larger connection ID.
For a network outage exercise, disable and restore your network while recording;
retries should cap at ten seconds and resume without restarting. Stop with Ctrl-C.
The legacy `--record FILE` spelling also accepts the fault option.

Recovery tests inject a monotonic clock, transport callbacks, and reconnect scheduling.
They do not sleep for health or backoff intervals. A premature update on connection 2
causes another recovery, so its replacement snapshot is tested on connection 3;
accepting that snapshot on the already abandoned connection would violate fail-closed
recovery. Each failure schedules exactly one retry.
