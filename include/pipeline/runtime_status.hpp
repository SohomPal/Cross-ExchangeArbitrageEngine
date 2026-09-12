#pragma once
#include "adapters/coinbase/coinbase_message_handler.hpp"
#include "recording/raw_event_recorder.hpp"
#include <condition_variable>
#include <filesystem>
#include <mutex>
namespace pipeline {
enum class RuntimeState {
    Starting,
    Running,
    Stopping,
    Stopped,
    StoppedRecordingError,
    StoppedQueueFull,
    StoppedProcessingError
};
struct RuntimeStatus {
    RuntimeState runtime_state{RuntimeState::Starting};
    bool connected{false};
    core::BookState book_state{core::BookState::Initializing};
    std::uint64_t received_messages{0}, enqueued_messages{0}, written_messages{0},
        processed_messages{0};
    std::size_t bid_levels{0}, ask_levels{0};
    std::size_t recording_queue_messages{0}, recording_queue_bytes{0};
    std::optional<std::uint64_t> last_sequence;
    std::optional<core::PriceTicks> best_bid, best_ask;
    std::optional<std::string> fatal_error;
    adapters::coinbase::RuntimeProgress progress;
};
struct SharedRuntime {
    mutable std::mutex mutex;
    std::condition_variable changed;
    RuntimeStatus status;
    bool writer_ready{false}, writer_done{false}, market_done{false};
    // Invoked under mutex, only to post onto the market io_context. Cleared before its destruction.
    std::function<void(std::string)> post_recording_failure;
    RuntimeStatus copy_status() const {
        std::lock_guard lock(mutex);
        return status;
    }
    void recording_failed(std::string error);
};
using AppendRaw = std::function<bool(recording::RawEventRecorder&, const recording::RawEnvelope&)>;
void write_raw(recording::RawRecordingQueue&, SharedRuntime&, const std::filesystem::path&,
               std::uintmax_t minimum_free_disk_bytes = 64 * 1024 * 1024, AppendRaw append = {});
std::string unpersisted_processed_range(const RuntimeStatus&);
int run_live(const std::filesystem::path&, int force_seconds, recording::RawQueueConfig = {},
             std::uintmax_t minimum_free_disk_bytes = 64 * 1024 * 1024);
} // namespace pipeline
