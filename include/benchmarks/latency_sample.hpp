#pragma once
#include <cstdint>
#include <cstddef>
namespace benchmarks {
struct LatencySample {
    std::int64_t nanoseconds;
    // Envelopes containing a snapshot (including mixed envelopes) belong to this category.
    bool snapshot;
    std::uint64_t record_index{0};
    std::size_t payload_bytes{0}, number_of_changes{0};
};
} // namespace benchmarks
