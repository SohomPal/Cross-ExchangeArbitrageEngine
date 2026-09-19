#include "adapters/kraken/kraken_l2_parser.hpp"
#include "adapters/kraken/kraken_symbol_mapper.hpp"
#include <chrono>
#include <limits>
#include <nlohmann/json.hpp>
#include <regex>
namespace adapters::kraken {
using nlohmann::json;
namespace {
core::ExchangeTimestamp parse_timestamp(std::string_view value) {
    const auto invalid = [] { throw std::invalid_argument("invalid Kraken UTC timestamp"); };
    if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
        invalid();
    }
    const auto number = [&](std::size_t offset, std::size_t length) {
        int result = 0;
        for (auto i = offset; i < offset + length; ++i) {
            if (value[i] < '0' || value[i] > '9') {
                invalid();
            }
            result = result * 10 + value[i] - '0';
        }
        return result;
    };
    const int year = number(0, 4);
    const std::chrono::year_month_day date{std::chrono::year{year},
                                           std::chrono::month{static_cast<unsigned>(number(5, 2))},
                                           std::chrono::day{static_cast<unsigned>(number(8, 2))}};
    const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
    if (year == 0 || !date.ok() || hour > 23 || minute > 59 || second > 59) {
        invalid();
    }
    std::int64_t fraction = 0;
    if (value.size() != 20) {
        if (value[19] != '.' || value.size() < 22 || value.size() > 30) {
            invalid();
        }
        const auto digits = value.size() - 21;
        fraction = number(20, digits);
        for (auto i = digits; i < 9; ++i) {
            fraction *= 10;
        }
    }
    const std::int64_t seconds =
        static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count()) * 86400 +
        hour * 3600 + minute * 60 + second;
    constexpr std::int64_t billion = 1000000000;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    // Handle the negative boundary without overflowing the intermediate product.
    if (seconds >= 0) {
        if (seconds > (maximum - fraction) / billion) {
            invalid();
        }
        return {seconds * billion + fraction};
    }
    if (seconds < minimum / billion - 1 ||
        (seconds == minimum / billion - 1 && fraction < billion + minimum % billion)) {
        invalid();
    }
    return {(seconds + 1) * billion + (fraction - billion)};
}

// Protect all original number tokens before DOM decoding. Strings remain strings;
// numeric values become a private tagged array, so quoted prices are rejected.
json exact_json(std::string_view raw) {
    std::string out;
    for (std::size_t i = 0; i < raw.size();) {
        char c = raw[i];
        if (c == '"') {
            auto start = i++;
            bool escaped = false, closed = false;
            while (i < raw.size()) {
                char v = raw[i++];
                if (escaped)
                    escaped = false;
                else if (v == '\\')
                    escaped = true;
                else if (v == '"') {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                throw std::invalid_argument("unterminated JSON string");
            auto token = raw.substr(start, i - start);
            if (json::parse(token) == "#number")
                throw std::invalid_argument("reserved numeric tag");
            out.append(token);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            auto start = i++;
            while (i < raw.size() &&
                   (std::isdigit(static_cast<unsigned char>(raw[i])) || raw[i] == '.' ||
                    raw[i] == 'e' || raw[i] == 'E' || raw[i] == '+' || raw[i] == '-'))
                ++i;
            std::string token(raw.substr(start, i - start));
            static const std::regex grammar(R"(-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
            if (!std::regex_match(token, grammar))
                throw std::invalid_argument("invalid JSON number");
            out += json::array({"#number", token}).dump();
        } else {
            out += c;
            ++i;
        }
    }
    return json::parse(out);
}
std::string number(const json& j) {
    if (!j.is_array() || j.size() != 2 || j[0] != "#number" || !j[1].is_string())
        throw std::invalid_argument("expected numeric token");
    return j[1].get<std::string>();
}
} // namespace
std::string decimal_token(std::string_view token) {
    static const std::regex grammar(R"((0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?)");
    if (!std::regex_match(token.begin(), token.end(), grammar))
        throw std::invalid_argument("invalid nonnegative decimal");
    std::string s(token);
    auto e = s.find_first_of("eE");
    if (e == std::string::npos)
        return s;
    auto exponent = std::stoi(s.substr(e + 1));
    if (exponent < -100 || exponent > 100)
        throw std::invalid_argument("decimal exponent out of range");
    s.resize(e);
    auto dot = s.find('.');
    int position = int(dot == std::string::npos ? s.size() : dot) + exponent;
    if (dot != std::string::npos)
        s.erase(dot, 1);
    if (position <= 0)
        return "0." + std::string(-position, '0') + s;
    if (position >= int(s.size()))
        return s + std::string(position - int(s.size()), '0');
    s.insert(position, 1, '.');
    return s;
}
ParseResult KrakenL2Parser::parse(std::string_view raw, core::ReceiveWallTimestamp wall,
                                  core::ReceiveMonotonicTimestamp mono) const {
    try {
        auto j = exact_json(raw);
        if (!j.is_object())
            throw std::invalid_argument("expected object");
        if (j.value("method", "") == "subscribe") {
            bool ok = j.at("success").get<bool>();
            if (ok && (j.at("result").at("channel") != "book" ||
                       j.at("result").at("symbol") != "BTC/USD"))
                throw std::invalid_argument("unexpected subscription acknowledgement");
            return {ok ? ParseKind::SubscriptionSuccess : ParseKind::SubscriptionFailure,
                    {},
                    ok ? "" : j.value("error", "subscription rejected")};
        }
        auto channel = j.at("channel").get<std::string>();
        if (channel == "heartbeat")
            return {ParseKind::Heartbeat, {}, {}};
        if (channel != "book")
            return {ParseKind::Ignored, {}, {}};
        auto type = j.at("type").get<std::string>();
        if (type != "snapshot" && type != "update")
            throw std::invalid_argument("unknown book type");
        auto& data = j.at("data");
        if (!data.is_array() || data.size() != 1)
            throw std::invalid_argument("expected one book");
        auto& b = data[0];
        auto checksum = number(b.at("checksum"));
        if (checksum.find_first_not_of("0123456789") != std::string::npos)
            throw std::invalid_argument("invalid checksum");
        auto crc = std::stoull(checksum);
        if (crc > UINT32_MAX)
            throw std::invalid_argument("checksum overflow");
        KrakenBookMessage m{type == "snapshot" ? KrakenBookMessageType::Snapshot
                                               : KrakenBookMessageType::Update,
                            map_symbol(b.at("symbol").get<std::string>()),
                            {},
                            static_cast<std::uint32_t>(crc),
                            parse_timestamp(b.at("timestamp").get<std::string>()),
                            wall,
                            mono};
        for (auto side : {core::Side::Bid, core::Side::Ask}) {
            const char* key = side == core::Side::Bid ? "bids" : "asks";
            if (!b.contains(key) && type == "update")
                continue;
            for (const auto& l : b.at(key).get_ref<const json::array_t&>()) {
                auto p = number(l.at("price")), q = number(l.at("qty"));
                auto price = core::parse_price(decimal_token(p), 2);
                auto qty = core::parse_quantity(decimal_token(q), 8);
                if (!price.raw() || (type == "snapshot" && !qty.raw()))
                    throw std::invalid_argument("zero snapshot level or price");
                m.changes.push_back({side, price, qty, p, q});
            }
        }
        return {ParseKind::Book, std::move(m), {}};
    } catch (const std::exception& e) {
        return {ParseKind::Error, {}, e.what()};
    }
}
} // namespace adapters::kraken
