#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/kraken/kraken_book_processor.hpp"
#include "adapters/kraken/kraken_subscription.hpp"
#include "doctest.h"
#include <fstream>
using namespace adapters::kraken;
static std::string fixture(const char* name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
    return {std::istreambuf_iterator<char>(in), {}};
}

TEST_CASE("exact tokens, canonical mapping, controls and ordered changes") {
    KrakenL2Parser p;
    auto r = p.parse(fixture("book_snapshot.json"), {123}, {456});
    REQUIRE(r.book);
    CHECK(r.book->instrument == core::Instrument::BTC_USD);
    CHECK(r.book->changes[3].price_lexeme == "45281.0");
    CHECK(r.book->changes[0].quantity_lexeme == "0.10000000");
    CHECK(r.book->changes[10].quantity_lexeme == "0.00100000");
    CHECK(r.book->receive_wall_time.nanoseconds == 123);
    CHECK(r.book->receive_monotonic_time.nanoseconds == 456);
    CHECK(r.book->exchange_time.nanoseconds == 1789516800123456000LL);
    CHECK(p.parse(fixture("book_update.json")).book->type == KrakenBookMessageType::Update);
    CHECK(p.parse(fixture("delete_level.json")).book->changes[0].quantity.raw() == 0);
    CHECK(p.parse(fixture("repeated_level_update.json")).book->changes.size() == 3);
    CHECK(p.parse(fixture("heartbeat.json")).kind == ParseKind::Heartbeat);
    CHECK(p.parse(fixture("subscription_success.json")).kind == ParseKind::SubscriptionSuccess);
    CHECK(p.parse(fixture("subscription_failure.json")).kind == ParseKind::SubscriptionFailure);
    auto bad = fixture("book_snapshot.json");
    bad.replace(bad.find("BTC/USD"), 7, "ETH/USD");
    CHECK(p.parse(bad).kind == ParseKind::Error);
    for (auto token :
         {"01", "-1", "NaN", "1.2.3", "1e999", "\"45283.5\"", "[\"#number\",\"45283.5\"]"}) {
        auto raw = fixture("book_snapshot.json");
        raw.replace(raw.find("45283.5"), 7, token);
        CHECK(p.parse(raw).kind == ParseKind::Error);
    }
    CHECK(decimal_token("1.000000e-3") == "0.001000000");
}

TEST_CASE("subscription requests use canonical Kraken protocol settings") {
    for (auto depth : {10, 25, 100, 500, 1000}) {
        auto j = nlohmann::json::parse(subscription(depth));
        CHECK(j["method"] == "subscribe");
        CHECK(j["params"]["channel"] == "book");
        CHECK(j["params"]["depth"] == depth);
        CHECK(j["params"]["snapshot"] == true);
        CHECK(j["params"]["symbol"] == nlohmann::json::array({"BTC/USD"}));
    }
    CHECK_THROWS(subscription(11));
    CHECK_THROWS(decimal_token("1e2garbage"));
    auto raw = fixture("book_snapshot.json");
    raw.replace(raw.find("0.10000000"), 10, "1.0000000e-1");
    auto result = KrakenL2Parser{}.parse(raw);
    REQUIRE(result.book);
    CHECK(result.book->changes[0].quantity.raw() == 10000000);
    CHECK(result.book->changes[0].quantity_lexeme == "1.0000000e-1");
}
