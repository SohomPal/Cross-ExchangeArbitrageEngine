#include "adapters/kraken/kraken_checksum.hpp"
#include "adapters/kraken/kraken_l2_parser.hpp"
#include <algorithm>
#include <boost/crc.hpp>
#include <set>
namespace adapters::kraken {
std::string checksum_digits(std::string_view token) {
    auto s = decimal_token(token);
    s.erase(std::remove(s.begin(), s.end(), '.'), s.end());
    auto first = s.find_first_not_of('0');
    return first == std::string::npos ? "" : s.substr(first);
}
void KrakenChecksumBook::apply(const KrakenLevelChange& c) {
    if ((c.side != core::Side::Bid && c.side != core::Side::Ask) || !c.price.raw() ||
        core::parse_price(decimal_token(c.price_lexeme), 2) != c.price ||
        core::parse_quantity(decimal_token(c.quantity_lexeme), 8) != c.quantity)
        throw std::invalid_argument("inconsistent Kraken level");
    auto update = [&](auto& levels) {
        if (!c.quantity.raw())
            levels.erase(c.price);
        else
            levels.insert_or_assign(c.price, c);
    };
    if (c.side == core::Side::Bid)
        update(bids_);
    else
        update(asks_);
}
void KrakenChecksumBook::replace_snapshot(const std::vector<KrakenLevelChange>& changes) {
    KrakenChecksumBook next;
    std::set<std::pair<core::Side, core::PriceTicks>> seen;
    for (const auto& c : changes) {
        if (!c.quantity.raw() || !seen.emplace(c.side, c.price).second)
            throw std::invalid_argument("invalid snapshot level");
        next.apply(c);
    }
    *this = std::move(next);
}
void KrakenChecksumBook::truncate(std::size_t depth) {
    auto trim = [&](auto& levels) {
        while (levels.size() > depth)
            levels.erase(std::prev(levels.end()));
    };
    trim(bids_);
    trim(asks_);
}
std::uint32_t KrakenChecksumBook::checksum() const {
    boost::crc_32_type crc;
    auto append = [&](const auto& levels) {
        unsigned n = 0;
        for (const auto& [p, c] : levels) {
            if (n++ == 10)
                break;
            auto s = checksum_digits(c.price_lexeme) + checksum_digits(c.quantity_lexeme);
            crc.process_bytes(s.data(), s.size());
        }
    };
    append(asks_);
    append(bids_);
    return crc.checksum();
}
std::vector<core::BookLevel> KrakenChecksumBook::levels() const {
    std::vector<core::BookLevel> result;
    auto append = [&](const auto& levels) {
        for (const auto& [p, c] : levels)
            result.push_back({c.side, p, c.quantity});
    };
    append(bids_);
    append(asks_);
    return result;
}
} // namespace adapters::kraken
