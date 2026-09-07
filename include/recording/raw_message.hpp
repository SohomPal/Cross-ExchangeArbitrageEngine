#pragma once

#include "core/timestamp.hpp"
#include "core/venue.hpp"

#include <cstdint>
#include <string>

namespace recording {

using core::ReceiveMonotonicTimestamp;
using core::ReceiveWallTimestamp;
using core::Venue;

struct RawMessage {
    std::uint64_t record_index;
    Venue venue;
    std::uint64_t connection_id;

    ReceiveWallTimestamp receive_wall_time;
    ReceiveMonotonicTimestamp receive_monotonic_time;

    std::string payload;
};

} // namespace recording
