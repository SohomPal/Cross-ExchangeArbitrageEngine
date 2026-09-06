# Cross-Exchange Arbitrage Engine

Minimal C++20 numerical foundation. No exchange connections or trading logic yet.
Requires CMake 3.20+ and a C++20 compiler. Tests use vendored doctest 2.4.12;
configuration and builds require no network access.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/arbitrage_engine
```

The executable prints price ticks `6214327` and quantity atoms `125000`.

`core::PriceTicks`, `core::QuantityAtoms`, `core::MoneyAtoms`, and `core::FeeRate`
are distinct, explicitly constructed, nonnegative `std::int64_t` wrappers.
They expose `raw()` and same-type comparisons; implicit conversions and
arithmetic are deliberately absent. `price + quantity` does not compile.

```cpp
#include "core/fixed_point.hpp"

auto price = core::parse_price("62143.27", 2);          // 6214327
auto quantity = core::parse_quantity("0.00125000", 8); // 125000
auto fee = core::parse_fee_rate("0.001", 6);           // 1000 = 0.1%
auto valid = core::require_increment(price, core::PriceTicks{1});
```

Parsing uses only integer operations: `raw = decimal value * 10^scale`.
Accepted syntax is `[0-9]+(\.[0-9]+)?`, including leading zeros. Missing
fractional digits are padded with zeros. Excess digits are rejected even if
they are zeros. Signs, whitespace, exponents, separators, empty strings, and
leading or trailing decimal points are rejected with `std::invalid_argument`.
Scales outside 0–18 throw `std::out_of_range`; values exceeding `INT64_MAX`
throw `std::overflow_error`. Negative raw construction also throws
`std::invalid_argument`. Fee rates are nonnegative fractions, with no imposed
maximum of 100%; rebates and signed money balances are outside this ticket.

Scale, currency, and instrument metadata are caller-owned and are not stored
in these wrappers. Keep them consistent when comparing values or validating
increments. Price ticks here mean units of the chosen decimal scale, which
may be smaller than a venue's tick size. `require_increment(value, increment)`
accepts only same-type arguments and rejects nonmultiples or a zero increment
with `std::invalid_argument`; it never rounds. For example, at scale 2 a venue
tick of `0.05` is `PriceTicks{5}`.

The test framework header is vendored from
[doctest v2.4.12](https://github.com/doctest/doctest/tree/v2.4.12) under the MIT
license, reproduced in the header.
