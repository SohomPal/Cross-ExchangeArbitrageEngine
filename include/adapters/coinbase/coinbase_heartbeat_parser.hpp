#pragma once
#include "core/timestamp.hpp"
#include "core/venue.hpp"
#include <optional>
#include <string_view>
namespace core {
// Connection event: intentionally routed outside MarketEvent.
struct Heartbeat {
    Venue venue;
    ReceiveWallTimestamp receive_wall_time;
    ReceiveMonotonicTimestamp receive_monotonic_time;
    std::optional<std::uint64_t> heartbeat_counter;
};
} // namespace core
namespace adapters::coinbase {
std::optional<core::Heartbeat> parse_heartbeat(std::string_view raw,
                                               core::ReceiveWallTimestamp wall,
                                               core::ReceiveMonotonicTimestamp mono);
}
