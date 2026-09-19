#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/kraken/kraken_book_processor.hpp"
#include "doctest.h"
#include <fstream>
using namespace adapters::kraken;
static std::string fixture(const char* name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name);
    return {std::istreambuf_iterator<char>(in), {}};
}

TEST_CASE("transactional validation, ordered updates and snapshot recovery") {
    KrakenBookProcessor p;
    REQUIRE(p.process(fixture("book_snapshot.json")));
    CHECK(p.book().state() == core::BookState::Valid);
    CHECK_FALSE(p.book().last_sequence());
    REQUIRE(p.process(fixture("repeated_level_update.json")));
    CHECK(p.checksum() == 3310070434u);
    auto bids = p.book().bids();
    CHECK_FALSE(p.process(fixture("checksum_mismatch.json")));
    CHECK(p.book().state() == core::BookState::Invalid);
    CHECK_FALSE(p.book().best_bid());
    CHECK(p.book().bids() == bids);
    CHECK_FALSE(p.process(fixture("book_update.json")));
    auto bad = fixture("book_snapshot.json");
    bad.replace(bad.find("3310070434"), 10, "1");
    CHECK_FALSE(p.process(bad));
    CHECK(p.book().bids() == bids);
    REQUIRE(p.process(fixture("book_snapshot.json")));
    CHECK(p.metrics()["kraken_recovery_snapshots"] == 1);
    REQUIRE(p.process(fixture("delete_level.json")));
    CHECK(p.book().best_bid()->price.raw() == 4528340);
    REQUIRE(p.process(fixture("book_snapshot.json")));
    REQUIRE(p.process(fixture("book_update.json")));
    CHECK(p.book().best_bid()->price.raw() == 4528400);
}
TEST_CASE("depth truncation and validation after every change") {
    auto m = *KrakenL2Parser{}.parse(fixture("book_snapshot.json")).book;
    for (int i = 0; i < 120; ++i)
        m.changes.push_back({core::Side::Bid, core::PriceTicks{10000 + i * 100},
                             core::QuantityAtoms{100000000}, std::to_string(100 + i),
                             "1.00000000"});
    KrakenBookProcessor p;
    REQUIRE(p.apply(m));
    CHECK(p.book().bids().size() == 100);
    m.type = KrakenBookMessageType::Update;
    REQUIRE(p.apply(m));
    CHECK(p.book().bids().size() == 100);
    p.stale();
    CHECK_FALSE(p.book().best_bid());
    CHECK_FALSE(p.apply(m));
}
TEST_CASE("active messages suppress heartbeat timeout and deterministic time metrics") {
    KrakenBookProcessor p;
    REQUIRE(p.process(fixture("book_snapshot.json"), {}, {100}));
    REQUIRE(p.process(fixture("heartbeat.json"), {}, {200}));
    REQUIRE(p.process(fixture("repeated_level_update.json"), {}, {10000000200}));
    CHECK(p.book().state() == core::BookState::Valid);
    CHECK(p.last_message->nanoseconds == 10000000200);
    CHECK(p.last_heartbeat->nanoseconds == 200);
    CHECK(p.metrics()["kraken_valid_time"] == 10000000100ULL);
}

TEST_CASE("canonical and decimal state reject inconsistent changes transactionally") {
    auto m = *KrakenL2Parser{}.parse(fixture("book_snapshot.json")).book;
    KrakenBookProcessor p;
    REQUIRE(p.apply(m));
    auto before = p.book().asks();
    m.changes[0].quantity_lexeme = "0.20000000";
    CHECK_FALSE(p.apply(m));
    CHECK(p.book().asks() == before);
    m = *KrakenL2Parser{}.parse(fixture("book_snapshot.json")).book;
    m.changes.push_back(m.changes.front());
    CHECK_FALSE(p.apply(m));
    CHECK(p.book().asks() == before);
}
TEST_CASE("both sides truncate and discarded levels cannot reappear") {
    auto m = *KrakenL2Parser{}.parse(fixture("book_snapshot.json")).book;
    for (int i = 0; i < 101; ++i) {
        m.changes.push_back({core::Side::Bid, core::PriceTicks{10000 + i * 100},
                             core::QuantityAtoms{100000000}, std::to_string(100 + i),
                             "1.00000000"});
        m.changes.push_back({core::Side::Ask, core::PriceTicks{5000000 + i * 100},
                             core::QuantityAtoms{100000000}, std::to_string(50000 + i),
                             "1.00000000"});
    }
    KrakenBookProcessor p;
    REQUIRE(p.apply(m));
    CHECK(p.book().bids().size() == 100);
    CHECK(p.book().asks().size() == 100);
    m.type = KrakenBookMessageType::Update;
    m.changes = {
        {core::Side::Ask, core::PriceTicks{5000000}, core::QuantityAtoms{0}, "50000", "0"}};
    REQUIRE(p.apply(m));
    CHECK(p.book().asks().size() == 99);
    CHECK(p.book().asks().count(core::PriceTicks{5010000}) == 0);
}
