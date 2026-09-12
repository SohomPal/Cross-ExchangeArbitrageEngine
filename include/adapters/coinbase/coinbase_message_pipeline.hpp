#pragma once
#include "adapters/coinbase/coinbase_l2_parser.hpp"
#include "core/order_book.hpp"
#include "core/sequence_tracker.hpp"
#include <functional>

namespace adapters::coinbase {
// The sink is called before any parsing. Replay uses an accepting sink.
class CoinbaseMessagePipeline {
  public:
    using Sink = std::function<bool(std::string_view, core::ReceiveWallTimestamp,
                                    core::ReceiveMonotonicTimestamp)>;
    explicit CoinbaseMessagePipeline(Sink sink) : sink_(std::move(sink)) {}
    void reset_connection() {
        stopped_ = false;
        error.clear();
        sequence_.reset();
        book.mark_initializing();
    }
    bool process(std::string_view raw, core::ReceiveWallTimestamp wall,
                 core::ReceiveMonotonicTimestamp monotonic) {
        if (stopped_)
            return false;
        if (!sink_(raw, wall, monotonic))
            return fail("recording failed");
        ++raw_messages;
        const auto result = parser_.parse(raw, wall, monotonic);
        if (result.status == ParseStatus::Error) {
            ++parse_errors;
            return fail(result.error);
        }
        if (result.sequence) {
            auto observed = sequence_.observe(*result.sequence);
            if (observed == core::SequenceResult::Duplicate ||
                observed == core::SequenceResult::OutOfOrder)
                return true;
            if (observed == core::SequenceResult::Gap)
                return fail("Coinbase envelope sequence gap");
        }
        if (result.status == ParseStatus::Parsed && book.state() != core::BookState::Valid &&
            (result.events.empty() ||
             !std::holds_alternative<core::BookSnapshot>(result.events.front())))
            return fail("L2 update before snapshot");
        if (!book.apply(result.events))
            return fail("book rejected event");
        return true;
    }
    bool fail(std::string_view reason) {
        book.invalidate();
        stopped_ = true;
        error = reason;
        return false;
    }
    core::OrderBook book{core::Venue::Coinbase, core::Instrument::BTC_USD};
    std::uint64_t raw_messages{0}, parse_errors{0};
    std::string error;

  private:
    core::SequenceTracker sequence_;
    Sink sink_;
    CoinbaseL2Parser parser_;
    bool stopped_{false};
};
} // namespace adapters::coinbase
