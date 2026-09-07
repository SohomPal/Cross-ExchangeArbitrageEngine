#pragma once
#include <cstdint>
#include <optional>
namespace core {
enum class SequenceResult { First, Expected, Duplicate, OutOfOrder, Gap };
class SequenceTracker {
  public:
    SequenceResult observe(std::uint64_t sequence);
    void reset() { last_.reset(); }
    [[nodiscard]] std::optional<std::uint64_t> last() const { return last_; }

  private:
    std::optional<std::uint64_t> last_;
};
} // namespace core
