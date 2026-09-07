#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/coinbase_feed_health.hpp"
#include "adapters/coinbase/coinbase_heartbeat_parser.hpp"
#include <doctest.h>
using namespace adapters::coinbase;
TEST_CASE("monotonic time, grace period, quiet instruments and independent heartbeats") {
    FeedHealth health;
    FeedHealthConfig config;
    health.reset({0});
    CHECK(health.failure({5000000000LL}, config).empty());
    CHECK(health.failure({5000000001LL}, config) == "No Coinbase messages");
    health.last_any_message = core::ReceiveMonotonicTimestamp{6000000000LL};
    CHECK(health.failure({6000000000LL}, config) == "Coinbase heartbeat timeout");
    health.last_heartbeat = core::ReceiveMonotonicTimestamp{6000000000LL};
    CHECK(health.failure({6000000000LL}, config).empty());
    config.max_instrument_message_age = std::chrono::milliseconds{5000};
    CHECK(health.failure({6000000000LL}, config) == "Coinbase instrument timeout");
    health.reset({7000000000LL});
    CHECK_FALSE(health.last_heartbeat);
    CHECK(health.failure({7000000000LL}, config).empty());
}
TEST_CASE("heartbeat normalization ignores wall clock jumps and L2 sequences") {
    auto raw =
        R"({"channel":"heartbeats","sequence_num":999,"events":[{"heartbeat_counter":"3049"}]})";
    auto a = parse_heartbeat(raw, {9000000000LL}, {1});
    auto b = parse_heartbeat(raw, {-9000000000LL}, {2});
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->heartbeat_counter == 3049);
    CHECK(b->receive_monotonic_time.nanoseconds == 2);
    CHECK_FALSE(parse_heartbeat(R"({"channel":"future"})", {0}, {0}));
    CHECK_THROWS(parse_heartbeat(
        R"({"channel":"heartbeats","events":[{"heartbeat_counter":"-1"}]})", {0}, {0}));
}
