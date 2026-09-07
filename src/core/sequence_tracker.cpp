#include "core/sequence_tracker.hpp"
namespace core {
SequenceResult SequenceTracker::observe(std::uint64_t sequence) {
    if (!last_) {
        last_ = sequence;
        return SequenceResult::First;
    }
    if (sequence == *last_)
        return SequenceResult::Duplicate;
    if (sequence < *last_)
        return SequenceResult::OutOfOrder;
    if (sequence - *last_ != 1)
        return SequenceResult::Gap;
    last_ = sequence;
    return SequenceResult::Expected;
}
} // namespace core
