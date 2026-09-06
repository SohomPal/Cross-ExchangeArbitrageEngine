#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/market_event.hpp"

#include <type_traits>

using namespace core;

TEST_CASE("UpdatePreservesCanonicalFields") {
    BookUpdate update{
        .venue = Venue::Coinbase,
        .instrument = Instrument::BTC_USD,
        .changes = {
            BookLevel{
                Side::Bid,
                PriceTicks{6214327},
                QuantityAtoms{125000}
            }
        },
        .exchange_time = ExchangeTimestamp{100},
        .receive_wall_time = ReceiveWallTimestamp{110},
        .receive_monotonic_time = ReceiveMonotonicTimestamp{50},
        .sequence = 42
    };

    CHECK(update.instrument == Instrument::BTC_USD);
    CHECK(update.exchange_time.nanoseconds == 100);
    CHECK(update.receive_wall_time.nanoseconds == 110);
    CHECK(update.receive_monotonic_time.nanoseconds == 50);
    CHECK(update.venue == Venue::Coinbase);
    CHECK(update.sequence == 42);
    CHECK(update.changes[0].price.raw() == 6214327);
}

TEST_CASE("BookSnapshotCanContainBidsAndAsks") {
    BookSnapshot snap{
        .venue = Venue::Kraken,
        .instrument = Instrument::BTC_USD,
        .levels = {
            BookLevel{Side::Bid, PriceTicks{1000}, QuantityAtoms{10}},
            BookLevel{Side::Ask, PriceTicks{2000}, QuantityAtoms{20}}
        },
        .exchange_time = ExchangeTimestamp{200},
        .receive_wall_time = ReceiveWallTimestamp{210},
        .receive_monotonic_time = ReceiveMonotonicTimestamp{150},
        .sequence = std::nullopt
    };
    CHECK_FALSE(snap.sequence.has_value());
    CHECK(snap.exchange_time.nanoseconds == 200);
    CHECK(snap.receive_wall_time.nanoseconds == 210);
    CHECK(snap.receive_monotonic_time.nanoseconds == 150);
    snap.sequence = 0;
    REQUIRE(snap.sequence.has_value());
    CHECK(*snap.sequence == 0);
    REQUIRE(snap.levels.size() == 2);
    CHECK(snap.levels[0].side == Side::Bid);
    CHECK(snap.levels[1].side == Side::Ask);
}

TEST_CASE("BookUpdateMultipleChanges") {
    BookUpdate update{
        .venue = Venue::Coinbase,
        .instrument = Instrument::BTC_USD,
        .changes = {
            BookLevel{Side::Bid, PriceTicks{1000}, QuantityAtoms{0}},
            BookLevel{Side::Ask, PriceTicks{2000}, QuantityAtoms{30}}
        },
        .exchange_time = ExchangeTimestamp{300},
        .receive_wall_time = ReceiveWallTimestamp{310},
        .receive_monotonic_time = ReceiveMonotonicTimestamp{250},
        .sequence = std::nullopt
    };
    CHECK_FALSE(update.sequence.has_value());
    REQUIRE(update.changes.size() == 2);
    CHECK(update.changes[0].quantity.raw() == 0); // deletion
    CHECK(update.changes[1].quantity.raw() == 30); // replace
}

TEST_CASE("MarketEventVariantHoldsSnapshotOrUpdate") {
    BookSnapshot snap{
        .venue = Venue::Coinbase,
        .instrument = Instrument::BTC_USD,
        .levels = {},
        .exchange_time = ExchangeTimestamp{1},
        .receive_wall_time = ReceiveWallTimestamp{2},
        .receive_monotonic_time = ReceiveMonotonicTimestamp{3},
        .sequence = 1
    };
    MarketEvent evt = snap;
    CHECK(std::holds_alternative<BookSnapshot>(evt));
    evt = BookUpdate{
        .venue = Venue::Kraken,
        .instrument = Instrument::BTC_USD,
        .changes = {},
        .exchange_time = ExchangeTimestamp{4},
        .receive_wall_time = ReceiveWallTimestamp{5},
        .receive_monotonic_time = ReceiveMonotonicTimestamp{6},
        .sequence = std::nullopt
    };
    CHECK(std::holds_alternative<BookUpdate>(evt));
}

static_assert(std::is_same_v<decltype(BookLevel::price), core::PriceTicks>);
static_assert(std::is_same_v<decltype(BookLevel::quantity), core::QuantityAtoms>);
static_assert(!std::is_same_v<PriceTicks, QuantityAtoms>);
static_assert(!std::is_constructible_v<PriceTicks, QuantityAtoms>);
static_assert(!std::is_constructible_v<QuantityAtoms, PriceTicks>);

template <typename Left, typename Right>
constexpr bool distinct_timestamps =
    !std::is_same_v<Left, Right> &&
    !std::is_constructible_v<Left, Right> &&
    !std::is_constructible_v<Right, Left>;

static_assert(distinct_timestamps<ExchangeTimestamp, ReceiveWallTimestamp>);
static_assert(distinct_timestamps<ExchangeTimestamp, ReceiveMonotonicTimestamp>);
static_assert(distinct_timestamps<ReceiveWallTimestamp, ReceiveMonotonicTimestamp>);

TEST_CASE("event equality compares every canonical field and vector order") {
    const BookSnapshot snapshot{
        Venue::Coinbase, Instrument::BTC_USD,
        {{Side::Bid, PriceTicks{100}, QuantityAtoms{10}},
         {Side::Ask, PriceTicks{101}, QuantityAtoms{20}}},
        ExchangeTimestamp{1}, ReceiveWallTimestamp{2}, ReceiveMonotonicTimestamp{3}, 42
    };
    const BookUpdate update{
        snapshot.venue, snapshot.instrument, snapshot.levels,
        snapshot.exchange_time, snapshot.receive_wall_time,
        snapshot.receive_monotonic_time, snapshot.sequence
    };
    auto check_equality = [](const auto& original) {
        auto copy = original;
        CHECK(copy == original);
        CHECK(MarketEvent{copy} == MarketEvent{original});
        SUBCASE("venue") { copy.venue = Venue::Kraken; }
        SUBCASE("exchange timestamp") { ++copy.exchange_time.nanoseconds; }
        SUBCASE("wall timestamp") { ++copy.receive_wall_time.nanoseconds; }
        SUBCASE("monotonic timestamp") { ++copy.receive_monotonic_time.nanoseconds; }
        SUBCASE("absent sequence") { copy.sequence.reset(); }
        SUBCASE("different sequence") { copy.sequence = 43; }
        auto& levels = [&]() -> auto& {
            if constexpr (std::is_same_v<std::decay_t<decltype(copy)>, BookSnapshot>) {
                return copy.levels;
            } else {
                return copy.changes;
            }
        }();
        SUBCASE("side") { levels[0].side = Side::Ask; }
        SUBCASE("price") { levels[0].price = PriceTicks{99}; }
        SUBCASE("quantity") { levels[0].quantity = QuantityAtoms{0}; }
        SUBCASE("level order") { std::swap(levels[0], levels[1]); }
        SUBCASE("level count") { levels.pop_back(); }
        CHECK_FALSE(copy == original);
        CHECK_FALSE(MarketEvent{copy} == MarketEvent{original});
    };
    SUBCASE("snapshot") { check_equality(snapshot); }
    SUBCASE("update") { check_equality(update); }
    CHECK_FALSE(MarketEvent{snapshot} == MarketEvent{update});
}
