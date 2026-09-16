#pragma once
#include "adapters/coinbase/coinbase_feed_health.hpp"
#include "adapters/coinbase/coinbase_l2_parser.hpp"
#include "core/clock.hpp"
#include "core/order_book.hpp"
#include "core/sequence_tracker.hpp"
#include "recording/raw_recording_queue.hpp"
#include <chrono>
#include <functional>

namespace adapters::coinbase {
enum class MessageOutcome {
    BookUpdated,
    Ignored,
    Duplicate,
    OutOfOrder,
    SequenceGap,
    ParseError,
    RecordingRejected,
    ApplyError
};
const char* outcome_name(MessageOutcome outcome);
struct ProcessResult {
    core::StageTimings stages;
    MessageOutcome outcome{MessageOutcome::Ignored};
    std::size_t canonical_event_count{0};
    std::size_t number_of_changes{0};
    bool contains_snapshot{false};
    std::optional<std::chrono::nanoseconds> latency;
    std::size_t snapshots{0}, updates{0};
    std::string error;
};
struct RuntimeProgress {
    std::optional<std::uint64_t> last_received_index, last_enqueued_index, last_written_index,
        last_processed_index;
};
// Shared functional processing; no recording side effects.
class CoinbaseMessageProcessor {
  public:
    CoinbaseMessageProcessor(core::OrderBook& book, core::SequenceTracker& sequence,
                             FeedHealth& health, core::Clock& clock,
                             CoinbaseSymbolMapper symbols = {})
        : book_(book), sequence_(sequence), health_(health), clock_(clock),
          parser_(std::move(symbols)) {}
    ProcessResult process(const recording::RawEnvelope& envelope);
    bool profile_stages{false};

  private:
    core::OrderBook& book_;
    core::SequenceTracker& sequence_;
    FeedHealth& health_;
    core::Clock& clock_;
    CoinbaseL2Parser parser_;
};
// All methods, book/sequence/health references and publish callbacks belong to the market thread.
class CoinbaseMessageHandler {
  public:
    using Sink = std::function<recording::RawRecordingQueue::PushResult(recording::RawEnvelopePtr)>;
    using Publish = std::function<void(const ProcessResult&, const RuntimeProgress&)>;
    CoinbaseMessageHandler(core::OrderBook& book, core::SequenceTracker& sequence,
                           FeedHealth& health, Sink sink, Publish publish = {})
        : book_(book), sink_(std::move(sink)), publish_(std::move(publish)),
          processor_(book, sequence, health, clock_) {}
    ProcessResult handle(std::string payload, core::ReceiveWallTimestamp wall,
                         core::ReceiveMonotonicTimestamp mono);
    bool profile_stages{false};
    std::uint64_t connection_id{0};
    RuntimeProgress progress;

  private:
    core::OrderBook& book_;
    Sink sink_;
    Publish publish_;
    core::LiveClock clock_;
    CoinbaseMessageProcessor processor_;
    std::uint64_t next_index_{0};
};
} // namespace adapters::coinbase
