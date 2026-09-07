#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "core/sequence_tracker.hpp"
#include <doctest.h>
#include <limits>
TEST_CASE("sequence continuity and reset without wraparound") {
    core::SequenceTracker tracker;
    using R = core::SequenceResult;
    CHECK_FALSE(tracker.last());
    CHECK(tracker.observe(100) == R::First);
    CHECK(tracker.observe(101) == R::Expected);
    CHECK(tracker.observe(101) == R::Duplicate);
    CHECK(tracker.observe(99) == R::OutOfOrder);
    CHECK(tracker.observe(103) == R::Gap);
    CHECK(tracker.last() == 101);
    tracker.reset();
    CHECK(tracker.observe(std::numeric_limits<std::uint64_t>::max()) == R::First);
    CHECK(tracker.observe(0) == R::OutOfOrder);
}
