#pragma once
#include "adapters/kraken/kraken_checksum.hpp"
#include "adapters/kraken/kraken_l2_parser.hpp"
#include "core/order_book.hpp"
#include <nlohmann/json.hpp>
namespace adapters::kraken {
class KrakenBookProcessor {
  public:
    explicit KrakenBookProcessor(std::size_t depth = 100);
    bool process(std::string_view, core::ReceiveWallTimestamp = {},
                 core::ReceiveMonotonicTimestamp = {});
    bool apply(const KrakenBookMessage&);
    void reset_connection();
    void stale();
    void invalidate() { book_.invalidate(); }
    void disconnect() { book_.mark_disconnected(); }
    void advance(core::ReceiveMonotonicTimestamp now);
    const core::OrderBook& book() const { return book_; }
    const nlohmann::json& metrics() const { return metrics_; }
    std::uint32_t checksum() const { return checksum_; }
    std::string outcome, error;
    std::optional<core::ReceiveMonotonicTimestamp> last_message, last_heartbeat;

  private:
    std::size_t depth_;
    KrakenL2Parser parser_;
    KrakenChecksumBook checksum_book_;
    core::OrderBook book_{core::Venue::Kraken, core::Instrument::BTC_USD};
    nlohmann::json metrics_;
    std::uint32_t checksum_{};
    bool had_snapshot_{false};
    std::optional<core::ReceiveMonotonicTimestamp> previous_time_;
};
} // namespace adapters::kraken
