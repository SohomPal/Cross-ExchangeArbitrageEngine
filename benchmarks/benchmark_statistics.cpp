#include "benchmarks/benchmark_statistics.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
namespace benchmarks {
Statistics statistics(std::vector<std::int64_t> samples) {
    Statistics s;
    s.samples = samples.size();
    if (samples.empty())
        return s;
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](std::size_t percent) {
        const auto rank =
            (samples.size() / 100) * percent + ((samples.size() % 100) * percent + 99) / 100;
        return samples[rank - 1];
    };
    s.min_ns = samples.front();
    s.max_ns = samples.back();
    s.mean_ns =
        static_cast<double>(std::accumulate(samples.begin(), samples.end(), 0.0L) / samples.size());
    s.p50_ns = percentile(50);
    s.p95_ns = percentile(95);
    s.p99_ns = percentile(99);
    return s;
}
} // namespace benchmarks
