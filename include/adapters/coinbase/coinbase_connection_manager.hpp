#pragma once
#include "adapters/coinbase/coinbase_feed_health.hpp"
#include "adapters/coinbase/coinbase_l2_parser.hpp"
#include "adapters/coinbase/coinbase_websocket_client.hpp"
#include "adapters/coinbase/reconnect_policy.hpp"
#include "core/order_book.hpp"
#include "core/sequence_tracker.hpp"
#include <boost/asio/steady_timer.hpp>

namespace adapters::coinbase {
struct RecoveryMetrics {
    std::uint64_t connections_started{0}, connections_succeeded{0}, disconnects{0},
        reconnect_attempts{0}, recovery_successes{0}, recovery_count{0}, snapshots_received{0},
        updates_received{0}, updates_before_snapshot{0};
    std::chrono::nanoseconds time_to_first_snapshot{0}, recovery_duration{0}, valid_time{0},
        stale_time{0}, invalid_time{0};
    std::string last_failure_reason;
};
// All entry points execute on the io_context thread. Inject transport and clock for tests.
class CoinbaseConnectionManager {
  public:
    using Clock = std::function<core::ReceiveMonotonicTimestamp()>;
    using Sink = std::function<bool(std::uint64_t, std::string_view, core::ReceiveWallTimestamp,
                                    core::ReceiveMonotonicTimestamp)>;
    using Connect =
        std::function<void(CoinbaseWebSocketClient::MessageHandler,
                           CoinbaseWebSocketClient::ErrorHandler, std::function<void()>)>;
    using Schedule = std::function<void(std::chrono::milliseconds, std::function<void()>)>;
    CoinbaseConnectionManager(boost::asio::io_context&, Sink, Connect, std::function<void()> close,
                              Clock clock, FeedHealthConfig = {}, Schedule schedule = {});
    ~CoinbaseConnectionManager();
    void start();
    void stop();
    void force_disconnect_for_test();
    void check_health();
    const core::OrderBook& book() const { return book_; }
    const FeedHealth& health() const { return health_; }
    const RecoveryMetrics& metrics() const { return metrics_; }
    std::uint64_t connection_id() const { return connection_id_; }
    bool connected() const { return connected_; }
    std::optional<std::uint64_t> sequence() const { return sequence_.last(); }

  private:
    void begin_connection();
    void handle_connected();
    bool handle_message(std::string_view, core::ReceiveWallTimestamp,
                        core::ReceiveMonotonicTimestamp);
    void recover(std::string, core::BookState);
    void tick();
    void transition(core::BookState);
    Schedule schedule_;
    Sink sink_;
    Connect connect_;
    std::function<void()> close_;
    Clock clock_;
    FeedHealthConfig config_;
    boost::asio::steady_timer health_timer_, reconnect_timer_;
    core::OrderBook book_{core::Venue::Coinbase, core::Instrument::BTC_USD};
    core::SequenceTracker sequence_;
    CoinbaseL2Parser parser_;
    FeedHealth health_;
    ReconnectPolicy backoff_;
    RecoveryMetrics metrics_;
    bool stopping_{true}, recovery_pending_{false}, connected_{false};
    std::uint64_t generation_{0}, connection_id_{0};
    core::ReceiveMonotonicTimestamp state_since_{0}, connected_at_{0};
    std::optional<core::ReceiveMonotonicTimestamp> recovery_since_;
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
};
} // namespace adapters::coinbase
