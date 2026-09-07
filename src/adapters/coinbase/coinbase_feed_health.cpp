#include "adapters/coinbase/coinbase_feed_health.hpp"
namespace adapters::coinbase {
void FeedHealth::reset(core::ReceiveMonotonicTimestamp now) {
    connected_at_ = now;
    last_any_message.reset();
    last_heartbeat.reset();
    last_l2_message.reset();
}
std::string FeedHealth::failure(core::ReceiveMonotonicTimestamp now,
                                const FeedHealthConfig& c) const {
    auto expired = [&](auto last, auto limit) {
        return now.nanoseconds - last.value_or(connected_at_).nanoseconds >
               std::chrono::duration_cast<std::chrono::nanoseconds>(limit).count();
    };
    if (expired(last_any_message, c.max_any_message_age))
        return "No Coinbase messages";
    if (expired(last_heartbeat, c.max_heartbeat_age))
        return "Coinbase heartbeat timeout";
    if (c.max_instrument_message_age && expired(last_l2_message, *c.max_instrument_message_age))
        return "Coinbase instrument timeout";
    return {};
}
} // namespace adapters::coinbase
