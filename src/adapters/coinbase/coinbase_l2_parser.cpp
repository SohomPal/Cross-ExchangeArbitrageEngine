#include "adapters/coinbase/coinbase_l2_parser.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace adapters::coinbase {
namespace {

using nlohmann::json;

const std::string& string_field(const json& object, const char* key) {
    return object.at(key).get_ref<const std::string&>();
}

core::ExchangeTimestamp parse_timestamp(std::string_view value) {
    const auto invalid = [] { throw std::invalid_argument("invalid Coinbase UTC timestamp"); };
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

} // namespace

CoinbaseL2Parser::CoinbaseL2Parser(CoinbaseSymbolMapper symbols) : symbols_(std::move(symbols)) {}

CoinbaseParseResult
CoinbaseL2Parser::parse(std::string_view raw_message, core::ReceiveWallTimestamp receive_wall_time,
                        core::ReceiveMonotonicTimestamp receive_monotonic_time) const {
    CoinbaseParseResult result{ParseStatus::Error, {}, {}, std::string{raw_message}};
    try {
        const auto message = json::parse(raw_message);
        if (!message.is_object())
            throw std::invalid_argument("expected message object");
        if (!message.contains("channel") && !message.contains("type"))
            throw std::invalid_argument("message requires channel or type");
        if (message.value("type", std::string{}) == "error" ||
            message.value("channel", std::string{}) == "error")
            throw std::invalid_argument("explicit Coinbase error: " + std::string(raw_message));
        if (message.value("channel", std::string{}) != "l2_data") {
            result.status = ParseStatus::Ignored;
            return result;
        }
        const auto exchange_time = parse_timestamp(string_field(message, "timestamp"));
        const auto& sequence = message.at("sequence_num");
        if (!sequence.is_number_unsigned() &&
            !(sequence.is_number_integer() && sequence.get<std::int64_t>() >= 0)) {
            throw std::invalid_argument("sequence_num must be a nonnegative uint64 integer");
        }
        const auto sequence_num = sequence.get<std::uint64_t>();
        const auto& events = message.at("events").get_ref<const json::array_t&>();
        std::vector<core::MarketEvent> parsed;
        for (const auto& event : events) {
            const auto& type = string_field(event, "type");
            if (type != "snapshot" && type != "update") {
                throw std::invalid_argument("unknown Coinbase L2 event type");
            }
            const auto config = symbols_.lookup(string_field(event, "product_id"));
            if (!config) {
                throw std::invalid_argument("unsupported Coinbase product");
            }
            std::vector<core::BookLevel> levels;
            for (const auto& update : event.at("updates").get_ref<const json::array_t&>()) {
                const auto& side = string_field(update, "side");
                if (side != "bid" && side != "offer") {
                    throw std::invalid_argument("unknown Coinbase L2 side");
                }
                (void)parse_timestamp(string_field(update, "event_time"));
                levels.push_back(
                    {side == "bid" ? core::Side::Bid : core::Side::Ask,
                     core::parse_price(string_field(update, "price_level"), config->price_scale),
                     core::parse_quantity(string_field(update, "new_quantity"),
                                          config->quantity_scale)});
            }
            if (type == "snapshot") {
                parsed.emplace_back(core::BookSnapshot{
                    core::Venue::Coinbase, config->instrument, std::move(levels), exchange_time,
                    receive_wall_time, receive_monotonic_time, sequence_num});
            } else {
                parsed.emplace_back(core::BookUpdate{
                    core::Venue::Coinbase, config->instrument, std::move(levels), exchange_time,
                    receive_wall_time, receive_monotonic_time, sequence_num});
            }
        }
        result.events = std::move(parsed);
        result.status = ParseStatus::Parsed;
    } catch (const json::exception& error) {
        result.error = error.what();
    } catch (const std::logic_error& error) {
        result.error = error.what();
    } catch (const std::overflow_error& error) {
        result.error = error.what();
    }
    return result;
}

} // namespace adapters::coinbase
