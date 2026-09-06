#pragma once

#include "core/fixed_point.hpp"
#include "core/instrument.hpp"
#include "core/side.hpp"
#include "core/timestamp.hpp"
#include "core/venue.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace core {

// Normalized quantities are absolute, never deltas: a positive quantity
// replaces the level at (side, price); zero deletes it. Adapters must convert
// venue-specific semantics to this convention before emitting an event.
struct BookLevel {
    Side side;
    PriceTicks price;
    QuantityAtoms quantity;

    bool operator==(const BookLevel&) const = default;
};

struct BookSnapshot {
    Venue venue;
    Instrument instrument;
    std::vector<BookLevel> levels;

    ExchangeTimestamp exchange_time;
    ReceiveWallTimestamp receive_wall_time;
    ReceiveMonotonicTimestamp receive_monotonic_time;

    std::optional<std::uint64_t> sequence;

    bool operator==(const BookSnapshot&) const = default;
};

struct BookUpdate {
    Venue venue;
    Instrument instrument;
    std::vector<BookLevel> changes;

    ExchangeTimestamp exchange_time;
    ReceiveWallTimestamp receive_wall_time;
    ReceiveMonotonicTimestamp receive_monotonic_time;

    std::optional<std::uint64_t> sequence;

    bool operator==(const BookUpdate&) const = default;
};

using MarketEvent = std::variant<BookSnapshot, BookUpdate>;

} // namespace core
