# Development Setup

## Requirements

- CMake 3.20 or newer.
- A C++20 compiler.

Dependencies are vendored, so configuration and builds require no network access.

## Build and test

Run from the repository root:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/arbitrage_engine
```

The executable currently demonstrates fixed-point parsing and prints:

```text
Price ticks: 6214327
Quantity atoms: 125000
```

It does not start a market data feed or execute trades.

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
