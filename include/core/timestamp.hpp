#pragma once

#include <cstdint>

namespace core {

struct ExchangeTimestamp {
    std::int64_t nanoseconds;

    bool operator==(const ExchangeTimestamp&) const = default;
};

struct ReceiveWallTimestamp {
    std::int64_t nanoseconds;

    bool operator==(const ReceiveWallTimestamp&) const = default;
};

struct ReceiveMonotonicTimestamp {
    std::int64_t nanoseconds;

    bool operator==(const ReceiveMonotonicTimestamp&) const = default;
};

} // namespace core
