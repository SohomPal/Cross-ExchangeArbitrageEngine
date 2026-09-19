#pragma once
#include "core/market_event.hpp"
#include <string>
namespace adapters::kraken {
struct KrakenLevelChange {
    core::Side side;
    core::PriceTicks price;
    core::QuantityAtoms quantity;
    std::string price_lexeme, quantity_lexeme;
};
enum class KrakenBookMessageType { Snapshot, Update };
struct KrakenBookMessage {
    KrakenBookMessageType type;
    core::Instrument instrument;
    std::vector<KrakenLevelChange> changes;
    std::uint32_t checksum;
    core::ExchangeTimestamp exchange_time;
    core::ReceiveWallTimestamp receive_wall_time;
    core::ReceiveMonotonicTimestamp receive_monotonic_time;
};
} // namespace adapters::kraken
