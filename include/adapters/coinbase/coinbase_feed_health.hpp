#pragma once
#include "core/timestamp.hpp"
#include <chrono>
#include <optional>
#include <string>
namespace adapters::coinbase {
struct FeedHealthConfig {
    std::chrono::milliseconds check_interval{1000};
    std::chrono::milliseconds max_any_message_age{5000};
    std::chrono::milliseconds max_heartbeat_age{5000};
    std::optional<std::chrono::milliseconds> max_instrument_message_age;
};
struct FeedHealth {
    std::optional<core::ReceiveMonotonicTimestamp> last_any_message, last_heartbeat,
        last_l2_message;
    std::uint64_t heartbeat_count{0}, heartbeat_timeouts{0}, sequence_gaps{0},
        duplicate_sequences{0}, out_of_order_sequences{0};
    void reset(core::ReceiveMonotonicTimestamp now);
    std::string failure(core::ReceiveMonotonicTimestamp now, const FeedHealthConfig& config) const;

  private:
    core::ReceiveMonotonicTimestamp connected_at_{0};
};
} // namespace adapters::coinbase
