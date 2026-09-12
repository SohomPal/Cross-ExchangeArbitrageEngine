#pragma once
#include <cstdint>
namespace benchmarks {
struct LatencySample {
    std::int64_t nanoseconds;
    // Envelopes containing a snapshot (including mixed envelopes) belong to this category.
    bool snapshot;
};
} // namespace benchmarks
