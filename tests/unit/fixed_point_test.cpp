#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/fixed_point.hpp"

#include <limits>
#include <string>
#include <type_traits>

using namespace core;

template <typename Left, typename Right>
concept Addable = requires(Left left, Right right) { left + right; };

template <typename Left, typename Right>
concept Comparable = requires(Left left, Right right) { left == right; };

static_assert(!Addable<PriceTicks, QuantityAtoms>);
static_assert(!Comparable<PriceTicks, QuantityAtoms>);
static_assert(!std::is_constructible_v<PriceTicks, QuantityAtoms>);
static_assert(!std::is_convertible_v<std::int64_t, PriceTicks>);
static_assert(!std::is_convertible_v<PriceTicks, std::int64_t>);
static_assert(!std::is_same_v<MoneyAtoms, QuantityAtoms>);
static_assert(!std::is_same_v<FeeRate, MoneyAtoms>);
static_assert(PriceTicks{1} < PriceTicks{2});

TEST_CASE("decimal parsing preserves exact units") {
    CHECK(parse_price("62143.27", 2).raw() == 6214327);
    CHECK(parse_quantity("0.00125000", 8).raw() == 125000);
    CHECK(parse_quantity("0.00000001", 8).raw() == 1);
    CHECK(parse_price("100", 2).raw() == 10000);
    CHECK(parse_price("1.2300", 4).raw() == 12300);
    CHECK(parse_price("1.2", 4).raw() == 12000);
    CHECK(parse_money("123.45", 2).raw() == 12345);
    CHECK(parse_fee_rate("0.001", 6).raw() == 1000);
    CHECK(parse_price("0001.20", 2).raw() == 120);
    CHECK(parse_quantity("0.000000000000000001", 18).raw() == 1);
}

TEST_CASE("zero and integer scales") {
    CHECK(parse_price("0", 0).raw() == 0);
    CHECK(parse_price("0", 18).raw() == 0);
    CHECK(parse_quantity("0.00000000", 8).raw() == 0);
    CHECK(parse_money("000.00", 2).raw() == 0);
    CHECK(parse_fee_rate("0", 6).raw() == 0);
    CHECK(parse_price("123", 0).raw() == 123);
}

TEST_CASE("excess precision is rejected including trailing zeros") {
    CHECK_THROWS_AS((void)parse_price("1.234", 2), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_price("1.2300", 2), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_quantity("0.000000001", 8), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_price("1.0", 0), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_price("1", -1), std::out_of_range);
    CHECK_THROWS_AS((void)parse_price("1", 19), std::out_of_range);
}

TEST_CASE("strict decimal grammar rejects malformed strings") {
    for (const auto input : {"", " ", " 1", "1 ", "+1", "-1", "-0", ".1",
                             "1.", ".", "1.2.3", "1e2", "1,000", "NaN", "inf",
                             "12a", "1\n", "0x10"}) {
        CAPTURE(input);
        CHECK_THROWS_AS((void)parse_price(input, 2), std::invalid_argument);
        CHECK_THROWS_AS((void)parse_quantity(input, 8), std::invalid_argument);
    }
    CHECK_THROWS_AS((void)parse_price(std::string_view("1\0", 2), 2), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_money("-1", 2), std::invalid_argument);
    CHECK_THROWS_AS((void)parse_fee_rate("-0.01", 4), std::invalid_argument);
}

TEST_CASE("direct construction rejects negative raw values") {
    CHECK_THROWS_AS((void)PriceTicks{-1}, std::invalid_argument);
    CHECK_THROWS_AS((void)QuantityAtoms{-1}, std::invalid_argument);
    CHECK_THROWS_AS((void)MoneyAtoms{-1}, std::invalid_argument);
    CHECK_THROWS_AS((void)FeeRate{-1}, std::invalid_argument);
}

TEST_CASE("integer limits are checked during parsing and scale padding") {
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    CHECK(parse_price("9223372036854775807", 0).raw() == maximum);
    CHECK(parse_money("92233720368547758.07", 2).raw() == maximum);
    CHECK(parse_quantity("9.223372036854775807", 18).raw() == maximum);
    CHECK_THROWS_AS((void)parse_price("9223372036854775808", 0), std::overflow_error);
    CHECK_THROWS_AS((void)parse_money("92233720368547758.08", 2), std::overflow_error);
    CHECK_THROWS_AS((void)parse_quantity("9.223372036854775808", 18), std::overflow_error);
    CHECK_THROWS_AS((void)parse_price("92233720368547759", 2), std::overflow_error);
    CHECK_THROWS_AS((void)parse_price("10", 18), std::overflow_error);
    CHECK_THROWS_AS((void)parse_price(std::string(1000, '9'), 0), std::overflow_error);
    CHECK(parse_price(std::string(1000, '0') + "1", 2).raw() == 100);
}

TEST_CASE("venue increments are enforced without rounding") {
    const auto tick = parse_price("0.05", 2);
    CHECK(require_increment(parse_price("1.25", 2), tick).raw() == 125);
    CHECK_THROWS_AS((void)require_increment(parse_price("1.23", 2), tick), std::invalid_argument);
    const auto lot = parse_quantity("0.00001000", 8);
    CHECK(require_increment(parse_quantity("0.00125000", 8), lot).raw() == 125000);
    CHECK_THROWS_AS((void)require_increment(parse_quantity("0.00125001", 8), lot), std::invalid_argument);
    CHECK(require_increment(PriceTicks{0}, tick).raw() == 0);
    CHECK_THROWS_AS((void)require_increment(PriceTicks{100}, PriceTicks{0}), std::invalid_argument);
}
