#pragma once
#include "core/timestamp.hpp"
#include <chrono>
namespace core {
class Clock {
  public:
    virtual ~Clock() = default;
    virtual ReceiveWallTimestamp wall_now() const = 0;
    virtual ReceiveMonotonicTimestamp monotonic_now() const = 0;
};
class LiveClock final : public Clock {
  public:
    ReceiveWallTimestamp wall_now() const override {
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()};
    }
    ReceiveMonotonicTimestamp monotonic_now() const override {
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()};
    }
};
class ReplayClock final : public Clock {
  public:
    void set(ReceiveWallTimestamp wall, ReceiveMonotonicTimestamp mono) {
        wall_ = wall;
        mono_ = mono;
    }
    ReceiveWallTimestamp wall_now() const override { return wall_; }
    ReceiveMonotonicTimestamp monotonic_now() const override { return mono_; }

  private:
    ReceiveWallTimestamp wall_{};
    ReceiveMonotonicTimestamp mono_{};
};
} // namespace core
