#include "adapters/coinbase/coinbase_connection_manager.hpp"
#include "adapters/coinbase/coinbase_heartbeat_parser.hpp"
#include <stdexcept>
namespace adapters::coinbase {
CoinbaseConnectionManager::CoinbaseConnectionManager(boost::asio::io_context& io, Sink sink,
                                                     Connect connect, std::function<void()> close,
                                                     Clock clock, FeedHealthConfig config,
                                                     Schedule schedule)
    : schedule_(std::move(schedule)), sink_(std::move(sink)), connect_(std::move(connect)),
      close_(std::move(close)), clock_(std::move(clock)), config_(config), health_timer_(io),
      reconnect_timer_(io) {
    if (config.check_interval.count() <= 0 || config.max_any_message_age.count() <= 0 ||
        config.max_heartbeat_age.count() <= 0 ||
        (config.max_instrument_message_age && config.max_instrument_message_age->count() <= 0))
        throw std::invalid_argument("health intervals must be positive");
    book_.mark_disconnected();
    state_since_ = clock_();
}
CoinbaseConnectionManager::~CoinbaseConnectionManager() {
    stop();
    lifetime_.reset();
}
void CoinbaseConnectionManager::transition(core::BookState state) {
    auto now = clock_();
    auto duration = std::chrono::nanoseconds{now.nanoseconds - state_since_.nanoseconds};
    if (book_.state() == core::BookState::Valid)
        metrics_.valid_time += duration;
    if (book_.state() == core::BookState::Stale)
        metrics_.stale_time += duration;
    if (book_.state() == core::BookState::Invalid)
        metrics_.invalid_time += duration;
    state_since_ = now;
    switch (state) {
    case core::BookState::Initializing:
        book_.mark_initializing();
        break;
    case core::BookState::Stale:
        book_.mark_stale();
        break;
    case core::BookState::Invalid:
        book_.invalidate();
        break;
    case core::BookState::Disconnected:
        book_.mark_disconnected();
        break;
    case core::BookState::Resyncing:
        book_.mark_resyncing();
        break;
    case core::BookState::Valid:
        break; // installed transactionally by snapshot
    }
}
void CoinbaseConnectionManager::start() {
    if (!stopping_)
        return;
    stopping_ = false;
    begin_connection();
    tick();
}
void CoinbaseConnectionManager::stop() {
    if (stopping_)
        return;
    stopping_ = true;
    ++generation_;
    recovery_pending_ = false;
    health_timer_.cancel();
    reconnect_timer_.cancel();
    close_();
    connected_ = false;
    sequence_.reset();
    transition(core::BookState::Disconnected);
}
void CoinbaseConnectionManager::begin_connection() {
    if (stopping_)
        return;
    recovery_pending_ = false;
    connected_ = false;
    transition(core::BookState::Resyncing);
    sequence_.reset();
    connected_at_ = clock_();
    ++metrics_.connections_started;
    const auto generation = ++generation_;
    std::weak_ptr<int> life = lifetime_;
    connect_(
        [this, life, generation](auto raw, auto wall, auto mono) {
            if (life.expired() || stopping_ || generation != generation_)
                return false;
            return handle_message(raw, wall, mono);
        },
        [this, life, generation](auto reason) {
            if (life.expired() || stopping_ || generation != generation_)
                return;
            recover(std::string(reason), core::BookState::Disconnected);
        },
        [this, life, generation] {
            if (life.expired() || stopping_ || generation != generation_)
                return;
            handle_connected();
        });
}
void CoinbaseConnectionManager::handle_connected() {
    if (connected_)
        return;
    connected_ = true;
    ++connection_id_;
    ++metrics_.connections_succeeded;
    connected_at_ = clock_();
    health_.reset(connected_at_);
    sequence_.reset();
    transition(core::BookState::Initializing);
}
void CoinbaseConnectionManager::recover(std::string reason, core::BookState state) {
    if (stopping_ || recovery_pending_)
        return;
    recovery_pending_ = true;
    ++generation_;
    transition(state);
    sequence_.reset();
    if (connected_)
        ++metrics_.disconnects;
    connected_ = false;
    ++metrics_.recovery_count;
    metrics_.last_failure_reason = std::move(reason);
    if (!recovery_since_)
        recovery_since_ = clock_();
    close_();
    ++metrics_.reconnect_attempts;
    auto delay = backoff_.next_delay();
    std::weak_ptr<int> life = lifetime_;
    const auto generation = generation_;
    auto resume = [this, life, generation] {
        if (life.expired() || stopping_ || generation != generation_)
            return;
        begin_connection();
    };
    if (schedule_)
        schedule_(delay, std::move(resume));
    else {
        reconnect_timer_.expires_after(delay);
        reconnect_timer_.async_wait([resume = std::move(resume)](auto ec) {
            if (!ec)
                resume();
        });
    }
}
bool CoinbaseConnectionManager::handle_message(std::string_view raw,
                                               core::ReceiveWallTimestamp wall,
                                               core::ReceiveMonotonicTimestamp mono) {
    if (!connected_)
        return false;
    try {
        if (!sink_(connection_id_, raw, wall, mono))
            throw std::runtime_error("recording failed");
        health_.last_any_message = mono;
        auto parsed = parser_.parse(raw, wall, mono);
        if (parsed.status == ParseStatus::Error)
            throw std::runtime_error(parsed.error);
        // The session sequence spans all channels; only L2 events reach the book.
        if (parsed.sequence) {
            auto result = sequence_.observe(*parsed.sequence);
            if (result == core::SequenceResult::Duplicate) {
                ++health_.duplicate_sequences;
                return true;
            }
            if (result == core::SequenceResult::OutOfOrder) {
                ++health_.out_of_order_sequences;
                return true;
            }
            if (result == core::SequenceResult::Gap) {
                ++health_.sequence_gaps;
                throw std::runtime_error("Coinbase envelope sequence gap");
            }
        }
        if (auto heartbeat = parse_heartbeat(raw, wall, mono)) {
            health_.last_heartbeat = heartbeat->receive_monotonic_time;
            ++health_.heartbeat_count;
            return true;
        }
        if (parsed.status == ParseStatus::Ignored)
            return true;
        health_.last_l2_message = mono;
        if (book_.state() != core::BookState::Valid &&
            (parsed.events.empty() ||
             !std::holds_alternative<core::BookSnapshot>(parsed.events.front()))) {
            ++metrics_.updates_before_snapshot;
            throw std::runtime_error("L2 update before snapshot");
        }
        auto staged = book_;
        std::uint64_t snapshots = 0, updates = 0;
        for (const auto& event : parsed.events) {
            if (std::holds_alternative<core::BookUpdate>(event) &&
                staged.state() != core::BookState::Valid) {
                ++metrics_.updates_before_snapshot;
                throw std::runtime_error("L2 update before snapshot");
            }
            if (!std::visit([&](const auto& e) { return staged.apply(e); }, event))
                throw std::runtime_error("book rejected event");
            if (std::holds_alternative<core::BookSnapshot>(event))
                ++snapshots;
            else
                ++updates;
        }
        const bool reconstructed = book_.state() != core::BookState::Valid && snapshots;
        if (reconstructed)
            transition(core::BookState::Valid);
        book_ = std::move(staged);
        metrics_.snapshots_received += snapshots;
        metrics_.updates_received += updates;
        if (reconstructed) {
            backoff_.reset();
            metrics_.time_to_first_snapshot =
                std::chrono::nanoseconds{clock_().nanoseconds - connected_at_.nanoseconds};
            if (recovery_since_) {
                ++metrics_.recovery_successes;
                metrics_.recovery_duration =
                    std::chrono::nanoseconds{clock_().nanoseconds - recovery_since_->nanoseconds};
                recovery_since_.reset();
            }
        }
        return true;
    } catch (const std::exception& e) {
        recover(e.what(), core::BookState::Invalid);
        return false;
    }
}
void CoinbaseConnectionManager::check_health() {
    if (stopping_)
        return;
    if (!connected_) {
        if (!recovery_pending_ && clock_().nanoseconds - connected_at_.nanoseconds > 30000000000LL)
            recover("Coinbase connection timeout", core::BookState::Disconnected);
        return;
    }
    auto reason = health_.failure(clock_(), config_);
    if (reason.empty()) {
        transition(book_.state());
        return;
    }
    if (reason == "Coinbase heartbeat timeout")
        ++health_.heartbeat_timeouts;
    recover(std::move(reason), core::BookState::Stale);
}
void CoinbaseConnectionManager::tick() {
    health_timer_.expires_after(config_.check_interval);
    std::weak_ptr<int> life = lifetime_;
    health_timer_.async_wait([this, life](auto ec) {
        if (life.expired() || ec || stopping_)
            return;
        check_health();
        tick();
    });
}
void CoinbaseConnectionManager::force_disconnect_for_test() {
    recover("forced local disconnect", core::BookState::Disconnected);
}
} // namespace adapters::coinbase
