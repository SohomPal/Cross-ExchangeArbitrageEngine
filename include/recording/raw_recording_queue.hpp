#pragma once
#include "recording/raw_message.hpp"
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace recording {
using RawEnvelope = RawMessage;
using RawEnvelopePtr = std::shared_ptr<const RawEnvelope>;
struct RawQueueConfig {
    std::size_t maximum_messages{4096};
    std::size_t maximum_bytes{64 * 1024 * 1024};
};
class RawRecordingQueue {
  public:
    enum class PushResult { Accepted, Full, Closed };
    explicit RawRecordingQueue(RawQueueConfig config = {}) : config_(config) {}
    PushResult try_push(RawEnvelopePtr message) {
        std::lock_guard lock(mutex_);
        if (closed_)
            return PushResult::Closed;
        if (messages_.size() >= config_.maximum_messages ||
            message->payload.size() > config_.maximum_bytes - bytes_)
            return PushResult::Full;
        messages_.push_back(std::move(message));
        bytes_ += messages_.back()->payload.size();
        available_.notify_one();
        return PushResult::Accepted;
    }
    std::optional<RawEnvelopePtr> wait_pop() {
        std::unique_lock lock(mutex_);
        available_.wait(lock, [&] { return closed_ || !messages_.empty(); });
        if (messages_.empty())
            return std::nullopt;
        auto message = std::move(messages_.front());
        messages_.pop_front();
        bytes_ -= message->payload.size();
        return message;
    }
    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        available_.notify_all();
    }
    std::size_t message_count() const {
        std::lock_guard lock(mutex_);
        return messages_.size();
    }
    std::size_t payload_bytes() const {
        std::lock_guard lock(mutex_);
        return bytes_;
    }

  private:
    RawQueueConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    std::deque<RawEnvelopePtr> messages_;
    std::size_t bytes_{0};
    bool closed_{false};
};
} // namespace recording
