#pragma once

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace core {

// Each tag creates a distinct type, with no implicit integer conversion.
template <typename Tag>
class FixedPoint {
public:
    explicit constexpr FixedPoint(std::int64_t raw) : raw_(raw) {
        if (raw < 0) {
            throw std::invalid_argument("fixed-point values must be nonnegative");
        }
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }
    auto operator<=>(const FixedPoint&) const = default;

private:
    std::int64_t raw_;
};

using PriceTicks = FixedPoint<struct PriceTag>;
using QuantityAtoms = FixedPoint<struct QuantityTag>;
using MoneyAtoms = FixedPoint<struct MoneyTag>;
using FeeRate = FixedPoint<struct FeeRateTag>;

// Grammar: [0-9]+(\.[0-9]+)?; scale must be in [0, 18].
// invalid_argument: malformed input, negatives, or excess fractional digits.
// out_of_range: unsupported scale. overflow_error: result exceeds INT64_MAX.
[[nodiscard]] PriceTicks parse_price(std::string_view decimal, int scale);
[[nodiscard]] QuantityAtoms parse_quantity(std::string_view decimal, int scale);
[[nodiscard]] MoneyAtoms parse_money(std::string_view decimal, int scale);
// A fee is a dimensionless fraction: "0.001" at scale 6 is 1000 (0.1%).
[[nodiscard]] FeeRate parse_fee_rate(std::string_view decimal, int scale);

// Explicit rejection policy: never silently round to a venue's increment.
// Both arguments must use the same scale and denomination.
template <typename Tag>
[[nodiscard]] constexpr FixedPoint<Tag> require_increment(
    FixedPoint<Tag> value, FixedPoint<Tag> increment) {
    if (increment.raw() == 0) {
        throw std::invalid_argument("increment must be positive");
    }
    if (value.raw() % increment.raw() != 0) {
        throw std::invalid_argument("value is not a multiple of the venue increment");
    }
    return value;
}

} // namespace core
