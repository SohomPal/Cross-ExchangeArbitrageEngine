#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/order_book.hpp"

#include <iterator>
#include <utility>

using namespace core;

namespace {
BookLevel level(Side side, std::int64_t price, std::int64_t quantity) {
    return {side, PriceTicks{price}, QuantityAtoms{quantity}};
}

BookSnapshot snapshot() {
    return {Venue::Coinbase, Instrument::BTC_USD,
            {level(Side::Bid, 99, 3), level(Side::Ask, 102, 4),
             level(Side::Bid, 100, 5), level(Side::Ask, 101, 6)},
            {}, {}, {}, 42};
}

BookUpdate update(std::vector<BookLevel> changes) {
    return {Venue::Coinbase, Instrument::BTC_USD, std::move(changes),
            {}, {}, {}, 43};
}
}

TEST_CASE("New book requires a snapshot") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    CHECK(book.venue() == Venue::Coinbase);
    CHECK(book.instrument() == Instrument::BTC_USD);
    CHECK(book.state() == BookState::Initializing);
    CHECK_FALSE(book.best_bid());
    CHECK_FALSE(book.best_ask());
    CHECK_FALSE(book.has_two_sided_market());
    CHECK_FALSE(book.last_sequence());
    CHECK_FALSE(book.apply(update({level(Side::Bid, 100, 1)})));
    CHECK(book.state() == BookState::Initializing);
    CHECK(book.bids().empty());
    CHECK_FALSE(book.last_sequence());
}

TEST_CASE("Snapshot orders both sides and exposes best levels") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    CHECK(book.state() == BookState::Valid);
    CHECK(book.last_sequence() == 42);
    CHECK(book.has_two_sided_market());
    REQUIRE(book.best_bid());
    REQUIRE(book.best_ask());
    CHECK(*book.best_bid() == level(Side::Bid, 100, 5));
    CHECK(*book.best_ask() == level(Side::Ask, 101, 6));
    CHECK(book.bids() == BidLevels{{PriceTicks{100}, QuantityAtoms{5}},
                                  {PriceTicks{99}, QuantityAtoms{3}}});
    CHECK(book.asks() == AskLevels{{PriceTicks{101}, QuantityAtoms{6}},
                                  {PriceTicks{102}, QuantityAtoms{4}}});
    CHECK(std::next(book.bids().begin())->first == PriceTicks{99});
    CHECK(std::next(book.asks().begin())->first == PriceTicks{102});
}

TEST_CASE("Update inserts replaces and deletes on both sides") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    REQUIRE(book.apply(update({level(Side::Bid, 100, 75),
                               level(Side::Bid, 99, 0),
                               level(Side::Bid, 98, 8),
                               level(Side::Ask, 101, 9),
                               level(Side::Ask, 102, 0),
                               level(Side::Ask, 103, 10),
                               level(Side::Bid, 97, 0),
                               level(Side::Ask, 104, 0)})));
    CHECK(book.bids() == BidLevels{{PriceTicks{100}, QuantityAtoms{75}},
                                  {PriceTicks{98}, QuantityAtoms{8}}});
    CHECK(book.asks() == AskLevels{{PriceTicks{101}, QuantityAtoms{9}},
                                  {PriceTicks{103}, QuantityAtoms{10}}});
    CHECK(book.last_sequence() == 43);
}

TEST_CASE("Repeated changes follow event order and crossing within a batch is allowed") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    REQUIRE(book.apply(update({level(Side::Bid, 102, 1),
                               level(Side::Bid, 102, 0),
                               level(Side::Bid, 102, 7),
                               level(Side::Ask, 101, 0),
                               level(Side::Ask, 102, 0),
                               level(Side::Ask, 103, 8)})));
    CHECK(*book.best_bid() == level(Side::Bid, 102, 7));
    CHECK(*book.best_ask() == level(Side::Ask, 103, 8));
}

TEST_CASE("Same price on opposite sides remains independent") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    auto snap = snapshot();
    snap.levels = {level(Side::Bid, 100, 1), level(Side::Ask, 100, 2)};
    REQUIRE(book.apply(snap));
    CHECK(*book.best_bid() == level(Side::Bid, 100, 1));
    CHECK(*book.best_ask() == level(Side::Ask, 100, 2));
    REQUIRE(book.apply(update({level(Side::Bid, 100, 0)})));
    CHECK_FALSE(book.best_bid());
    CHECK(*book.best_ask() == level(Side::Ask, 100, 2));
    CHECK_FALSE(book.has_two_sided_market());
    CHECK(book.state() == BookState::Valid);
}

TEST_CASE("Replacement snapshots allow empty and one sided books") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    auto replacement = snapshot();
    replacement.levels.clear();
    replacement.sequence.reset();
    SUBCASE("empty") {}
    SUBCASE("bid only") { replacement.levels = {level(Side::Bid, 80, 1)}; }
    SUBCASE("ask only") { replacement.levels = {level(Side::Ask, 90, 2)}; }
    REQUIRE(book.apply(replacement));
    CHECK(book.state() == BookState::Valid);
    CHECK_FALSE(book.has_two_sided_market());
    CHECK_FALSE(book.last_sequence());
    CHECK(book.bids().count(PriceTicks{100}) == 0);
    CHECK(book.asks().count(PriceTicks{101}) == 0);
    CHECK(book.best_bid().has_value() == !book.bids().empty());
    CHECK(book.best_ask().has_value() == !book.asks().empty());
    CHECK(book.bids().size() + book.asks().size() == replacement.levels.size());
}

TEST_CASE("Malformed snapshots preserve all existing state") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    const auto before = book;
    auto bad = snapshot();
    bad.sequence = 999;
    SUBCASE("wrong venue") { bad.venue = Venue::Kraken; }
    SUBCASE("wrong instrument") { bad.instrument = static_cast<Instrument>(99); }
    SUBCASE("zero price") { bad.levels.push_back(level(Side::Bid, 0, 1)); }
    SUBCASE("zero quantity") { bad.levels.push_back(level(Side::Ask, 110, 0)); }
    SUBCASE("invalid side") { bad.levels.push_back(level(static_cast<Side>(99), 110, 1)); }
    SUBCASE("duplicate bid") { bad.levels.push_back(level(Side::Bid, 100, 7)); }
    SUBCASE("duplicate ask") { bad.levels.push_back(level(Side::Ask, 101, 7)); }
    CHECK_FALSE(book.apply(bad));
    CHECK(book.bids() == before.bids());
    CHECK(book.asks() == before.asks());
    CHECK(book.last_sequence() == before.last_sequence());
    CHECK(book.state() == before.state());
}

TEST_CASE("Malformed updates preserve all existing state after earlier changes") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    REQUIRE(book.apply(snapshot()));
    const auto before = book;
    auto bad = update({level(Side::Bid, 100, 0), level(Side::Ask, 101, 20)});
    SUBCASE("wrong venue") { bad.venue = Venue::Kraken; }
    SUBCASE("wrong instrument") { bad.instrument = static_cast<Instrument>(99); }
    SUBCASE("zero price") { bad.changes.push_back(level(Side::Ask, 0, 1)); }
    SUBCASE("zero deletion price") { bad.changes.push_back(level(Side::Bid, 0, 0)); }
    SUBCASE("invalid side") { bad.changes.push_back(level(static_cast<Side>(99), 110, 1)); }
    CHECK_FALSE(book.apply(bad));
    CHECK(book.bids() == before.bids());
    CHECK(book.asks() == before.asks());
    CHECK(book.last_sequence() == before.last_sequence());
    CHECK(book.state() == before.state());
}

TEST_CASE("Sequence metadata follows accepted events without generic enforcement") {
    OrderBook book{Venue::Kraken, Instrument::BTC_USD};
    auto snap = snapshot();
    snap.venue = Venue::Kraken;
    REQUIRE(book.apply(snap));
    auto change = update({});
    change.venue = Venue::Kraken;
    for (const auto sequence : {std::optional<std::uint64_t>{1000},
                                std::optional<std::uint64_t>{0},
                                std::optional<std::uint64_t>{0}, std::optional<std::uint64_t>{}}) {
        change.sequence = sequence;
        REQUIRE(book.apply(change));
        CHECK(book.last_sequence() == sequence);
        CHECK(book.bids().size() == 2);
        CHECK(book.asks().size() == 2);
    }
}

TEST_CASE("Invalid first snapshot leaves book initializing") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    auto bad = snapshot();
    bad.levels.push_back(level(Side::Ask, 110, 0));
    CHECK_FALSE(book.apply(bad));
    CHECK(book.state() == BookState::Initializing);
    CHECK(book.bids().empty());
    CHECK(book.asks().empty());
    CHECK_FALSE(book.last_sequence());
    REQUIRE(book.apply(snapshot()));
    CHECK(book.state() == BookState::Valid);
}
