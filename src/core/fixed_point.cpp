#include "core/fixed_point.hpp"

#include <limits>

namespace core {
namespace {

std::int64_t parse_decimal(std::string_view decimal, int scale) {
    if (scale < 0 || scale > 18) {
        throw std::out_of_range("scale must be between 0 and 18");
    }
    if (decimal.empty()) {
        throw std::invalid_argument("decimal must not be empty");
    }

    // Validate the entire string before accumulating, so malformed input
    // is consistently rejected even if an earlier prefix would overflow.
    bool point_seen = false;
    std::size_t fractional_digits = 0;
    for (std::size_t i = 0; i < decimal.size(); ++i) {
        const char ch = decimal[i];
        if (ch == '.') {
            if (point_seen || i == 0 || i == decimal.size() - 1) {
                throw std::invalid_argument("invalid decimal point");
            }
            point_seen = true;
        } else if (ch < '0' || ch > '9') {
            throw std::invalid_argument("decimal contains an invalid character");
        } else if (point_seen) {
            ++fractional_digits;
        }
    }
    if (fractional_digits > static_cast<std::size_t>(scale)) {
        throw std::invalid_argument("decimal exceeds requested precision");
    }

    std::int64_t raw = 0;
    const auto append = [&raw](int digit) {
        constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
        if (raw > (maximum - digit) / 10) {
            throw std::overflow_error("decimal exceeds INT64_MAX");
        }
        raw = raw * 10 + digit;
    };
    for (const char ch : decimal) {
        if (ch != '.') {
            append(ch - '0');
        }
    }
    for (auto i = fractional_digits; i < static_cast<std::size_t>(scale); ++i) {
        append(0);
    }
    return raw;
}

} // namespace

PriceTicks parse_price(std::string_view decimal, int scale) {
    return PriceTicks{parse_decimal(decimal, scale)};
}

QuantityAtoms parse_quantity(std::string_view decimal, int scale) {
    return QuantityAtoms{parse_decimal(decimal, scale)};
}

MoneyAtoms parse_money(std::string_view decimal, int scale) {
    return MoneyAtoms{parse_decimal(decimal, scale)};
}

FeeRate parse_fee_rate(std::string_view decimal, int scale) {
    return FeeRate{parse_decimal(decimal, scale)};
}

} // namespace core
