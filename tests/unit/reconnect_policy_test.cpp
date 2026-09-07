#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/reconnect_policy.hpp"
#include <doctest.h>
TEST_CASE("bounded exponential backoff and explicit reset") {
    adapters::coinbase::ReconnectPolicy policy;
    for (int expected : {250, 500, 1000, 2000, 4000, 8000, 10000, 10000})
        CHECK(policy.next_delay().count() == expected);
    for (int i = 0; i < 1000; ++i)
        CHECK(policy.next_delay().count() == 10000);
    policy.reset();
    CHECK(policy.next_delay().count() == 250);
}
