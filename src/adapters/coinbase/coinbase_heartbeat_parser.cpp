#include "adapters/coinbase/coinbase_heartbeat_parser.hpp"
#include <charconv>
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace adapters::coinbase {
std::optional<core::Heartbeat> parse_heartbeat(std::string_view raw,
                                               core::ReceiveWallTimestamp wall,
                                               core::ReceiveMonotonicTimestamp mono) {
    const auto j = nlohmann::json::parse(raw);
    if (j.value("channel", std::string{}) != "heartbeats")
        return {};
    core::Heartbeat result{core::Venue::Coinbase, wall, mono, {}};
    const auto& events = j.at("events");
    if (!events.is_array() || events.empty())
        throw std::invalid_argument("invalid heartbeat events");
    for (const auto& e : events) {
        if (!e.is_object())
            throw std::invalid_argument("invalid heartbeat event");
        if (!e.contains("heartbeat_counter"))
            continue;
        const auto& v = e.at("heartbeat_counter");
        if (v.is_number_unsigned())
            result.heartbeat_counter = v.get<std::uint64_t>();
        else if (v.is_string()) {
            const auto s = v.get<std::string>();
            std::uint64_t counter;
            auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), counter);
            if (ec != std::errc{} || end != s.data() + s.size())
                throw std::invalid_argument("invalid heartbeat counter");
            result.heartbeat_counter = counter;
        } else
            throw std::invalid_argument("invalid heartbeat counter");
    }
    return result;
}
} // namespace adapters::coinbase
