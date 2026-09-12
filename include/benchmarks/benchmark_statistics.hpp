#pragma once
#include <cstdint>
#include <vector>
namespace benchmarks {
struct Statistics {
    std::size_t samples{0};
    std::int64_t min_ns{0}, p50_ns{0}, p95_ns{0}, p99_ns{0}, max_ns{0};
    double mean_ns{0};
};
// Nearest rank: sorted[ceil(percentile * N) - 1]. Empty sets have zero-valued statistics.
Statistics statistics(std::vector<std::int64_t> samples);
} // namespace benchmarks
