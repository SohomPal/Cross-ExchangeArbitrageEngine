#include "adapters/kraken/kraken_book_processor.hpp"
#include "adapters/kraken/kraken_subscription.hpp"
namespace adapters::kraken {
KrakenBookProcessor::KrakenBookProcessor(std::size_t depth) : depth_(depth) {
    validate_depth(depth);
    for (auto key : {"messages", "snapshots", "updates", "heartbeats", "subscription_errors",
                     "parse_errors", "checksum_validations", "checksum_failures", "reconnects",
                     "recovery_snapshots", "valid_time", "invalid_time", "stale_time"})
        metrics_[std::string("kraken_") + key] = std::uint64_t{0};
}
void KrakenBookProcessor::advance(core::ReceiveMonotonicTimestamp now) {
    if (previous_time_ && now.nanoseconds >= previous_time_->nanoseconds) {
        const char* key = book_.state() == core::BookState::Valid   ? "kraken_valid_time"
                          : book_.state() == core::BookState::Stale ? "kraken_stale_time"
                                                                    : "kraken_invalid_time";
        metrics_[key] =
            metrics_[key].get<std::uint64_t>() + (now.nanoseconds - previous_time_->nanoseconds);
    }
    previous_time_ = now;
}
void KrakenBookProcessor::reset_connection() {
    ++metrics_["kraken_reconnects"].get_ref<nlohmann::json::number_unsigned_t&>();
    book_.mark_initializing();
    last_message.reset();
    last_heartbeat.reset();
}
void KrakenBookProcessor::stale() { book_.mark_stale(); }
bool KrakenBookProcessor::process(std::string_view raw, core::ReceiveWallTimestamp wall,
                                  core::ReceiveMonotonicTimestamp mono) {
    advance(mono);
    last_message = mono;
    auto inc = [&](const char* key) {
        ++metrics_[key].get_ref<nlohmann::json::number_unsigned_t&>();
    };
    inc("kraken_messages");
    error.clear();
    auto r = parser_.parse(raw, wall, mono);
    switch (r.kind) {
    case ParseKind::Heartbeat:
        inc("kraken_heartbeats");
        last_heartbeat = mono;
        outcome = "Heartbeat";
        return true;
    case ParseKind::SubscriptionSuccess:
        outcome = "SubscriptionSuccess";
        return true;
    case ParseKind::Ignored:
        outcome = "Ignored";
        return true;
    case ParseKind::SubscriptionFailure:
        inc("kraken_subscription_errors");
        outcome = "SubscriptionFailure";
        break;
    case ParseKind::Error:
        inc("kraken_parse_errors");
        outcome = "ParseError";
        break;
    case ParseKind::Book:
        return apply(*r.book);
    }
    error = r.error;
    book_.invalidate();
    return false;
}
bool KrakenBookProcessor::apply(const KrakenBookMessage& m) {
    auto inc = [&](const char* key) {
        ++metrics_[key].get_ref<nlohmann::json::number_unsigned_t&>();
    };
    bool snapshot = m.type == KrakenBookMessageType::Snapshot;
    inc(snapshot ? "kraken_snapshots" : "kraken_updates");
    if (!snapshot && book_.state() != core::BookState::Valid) {
        outcome = "AwaitingSnapshot";
        return false;
    }
    try {
        if (m.instrument != core::Instrument::BTC_USD)
            throw std::invalid_argument("wrong instrument");
        auto next = checksum_book_;
        if (snapshot)
            next.replace_snapshot(m.changes);
        else
            for (const auto& c : m.changes)
                next.apply(c);
        next.truncate(depth_);
        auto crc = next.checksum();
        inc("kraken_checksum_validations");
        if (crc != m.checksum) {
            inc("kraken_checksum_failures");
            outcome = "ChecksumMismatch";
            error = "Kraken checksum mismatch";
            book_.invalidate();
            return false;
        }
        core::OrderBook candidate(core::Venue::Kraken, m.instrument);
        if (!candidate.apply(core::BookSnapshot{core::Venue::Kraken,
                                                m.instrument,
                                                next.levels(),
                                                m.exchange_time,
                                                m.receive_wall_time,
                                                m.receive_monotonic_time,
                                                {}}))
            throw std::invalid_argument("invalid canonical snapshot");
        if (candidate.has_two_sided_market() &&
            candidate.best_bid()->price >= candidate.best_ask()->price)
            throw std::invalid_argument("crossed Kraken book");
        if (snapshot && had_snapshot_)
            inc("kraken_recovery_snapshots");
        had_snapshot_ = true;
        book_ = std::move(candidate);
        checksum_book_ = std::move(next);
        checksum_ = crc;
        outcome = snapshot ? "Snapshot" : "Update";
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        outcome = "ApplyError";
        book_.invalidate();
        return false;
    }
}
} // namespace adapters::kraken
