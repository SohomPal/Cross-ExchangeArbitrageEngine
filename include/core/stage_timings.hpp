#pragma once
#include <chrono>
#include <cstdint>
namespace core {
struct StageTimings {
    std::uint64_t enqueue_ns{0}, json_parse_ns{0}, schema_validation_ns{0},
        fixed_point_ns{0}, canonical_event_ns{0}, sequence_validation_ns{0},
        book_apply_ns{0}, status_publish_ns{0};
};
// A null destination performs no clock reads. Profiling is opt-in per handler.
class StageTimer {
  public:
    explicit StageTimer(std::uint64_t* destination) : destination_(destination) {
        if (destination_)
            start_ = std::chrono::steady_clock::now();
    }
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
    ~StageTimer() { stop(); }
    void stop() {
        if (destination_) {
            *destination_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start_).count();
            destination_ = nullptr;
        }
    }
  private:
    std::uint64_t* destination_;
    std::chrono::steady_clock::time_point start_;
};
} // namespace core
