#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <nlohmann/json.hpp>

#include "adapters/coinbase/coinbase_l2_parser.hpp"
#include "core/order_book.hpp"

#include <fstream>
#include <iterator>
#include <limits>

using namespace core;
using namespace adapters::coinbase;
using nlohmann::json;

namespace {
std::string fixture(const char* name) {
    std::ifstream file{std::string{FIXTURE_DIR} + "/" + name + ".json"};
    if (!file) {
        throw std::runtime_error("fixture unavailable");
    }
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
CoinbaseParseResult parse(const std::string& raw) {
    return CoinbaseL2Parser{}.parse(raw, ReceiveWallTimestamp{100}, ReceiveMonotonicTimestamp{50});
}
void expect_error(const std::string& raw) {
    const auto result = parse(raw);
    CHECK(result.status == ParseStatus::Error);
    CHECK(result.events.empty());
    CHECK_FALSE(result.error.empty());
    CHECK(result.raw_message == raw);
}
} // namespace

TEST_CASE("snapshot preserves exact canonical fields and raw envelope") {
    const auto raw = fixture("l2_snapshot");
    const auto result = parse(raw);
    REQUIRE(result.status == ParseStatus::Parsed);
    CHECK(result.error.empty());
    CHECK(result.raw_message == raw);
    REQUIRE(result.events.size() == 1);
    const BookSnapshot expected{Venue::Coinbase,
                                Instrument::BTC_USD,
                                {{Side::Bid, PriceTicks{6214327}, QuantityAtoms{125000}},
                                 {Side::Bid, PriceTicks{6214200}, QuantityAtoms{200000000}},
                                 {Side::Ask, PriceTicks{6214400}, QuantityAtoms{300000000}},
                                 {Side::Ask, PriceTicks{6214500}, QuantityAtoms{400000000}}},
                                ExchangeTimestamp{1788726601123456789},
                                ReceiveWallTimestamp{100},
                                ReceiveMonotonicTimestamp{50},
                                42};
    CHECK(std::get<BookSnapshot>(result.events[0]) == expected);
}

TEST_CASE("snapshot updates and deletions reconstruct every book level") {
    OrderBook book{Venue::Coinbase, Instrument::BTC_USD};
    for (const auto* name : {"l2_snapshot", "l2_update", "l2_delete", "l2_multiple_changes"}) {
        const auto result = parse(fixture(name));
        REQUIRE(result.status == ParseStatus::Parsed);
        for (const auto& event : result.events) {
            std::visit([&](const auto& value) { REQUIRE(book.apply(value)); }, event);
        }
        if (std::string_view{name} == "l2_update") {
            CHECK(book.bids() == BidLevels{{PriceTicks{6214327}, QuantityAtoms{50000000}},
                                           {PriceTicks{6214200}, QuantityAtoms{200000000}}});
            CHECK(book.asks() == AskLevels{{PriceTicks{6214450}, QuantityAtoms{100000000}},
                                           {PriceTicks{6214500}, QuantityAtoms{400000000}}});
        }
    }
    CHECK(book.bids() == BidLevels{{PriceTicks{6214200}, QuantityAtoms{600000000}}});
    CHECK(book.asks() == AskLevels{{PriceTicks{6214450}, QuantityAtoms{100000000}}});
    CHECK(book.last_sequence() == 45);
}

TEST_CASE("unrelated channels are ignored and malformed messages are atomic") {
    CHECK(parse(fixture("unsupported_channel")).status == ParseStatus::Ignored);
    CHECK(parse(R"({"channel":"heartbeats"})").events.empty());
    expect_error("{");
    expect_error("[]");
    expect_error(fixture("malformed_l2"));
    auto message = json::parse(fixture("l2_snapshot"));
    message["events"].push_back(json::parse(fixture("malformed_l2"))["events"][0]);
    expect_error(message.dump());
}

TEST_CASE("multiple events preserve order and envelope metadata") {
    auto message = json::parse(fixture("l2_snapshot"));
    message["events"].push_back(json::parse(fixture("l2_update"))["events"][0]);
    const auto result = parse(message.dump());
    REQUIRE(result.status == ParseStatus::Parsed);
    REQUIRE(result.events.size() == 2);
    CHECK(std::holds_alternative<BookSnapshot>(result.events[0]));
    const auto& update = std::get<BookUpdate>(result.events[1]);
    CHECK(update.sequence == 42);
    CHECK(update.exchange_time == ExchangeTimestamp{1788726601123456789});
    CHECK(update.receive_wall_time == ReceiveWallTimestamp{100});
    CHECK(update.receive_monotonic_time == ReceiveMonotonicTimestamp{50});
    CHECK(update.changes.size() == 3);
    message["events"] = json::array();
    CHECK(parse(message.dump()).status == ParseStatus::Parsed);
}

TEST_CASE("required fields and types are validated at each nesting level") {
    const auto original = json::parse(fixture("l2_update"));
    for (const auto* field : {"channel", "timestamp", "sequence_num", "events"}) {
        auto message = original;
        message.erase(field);
        expect_error(message.dump());
        message = original;
        message[field] = nullptr;
        expect_error(message.dump());
    }
    for (const auto* field : {"type", "product_id", "updates"}) {
        auto message = original;
        message["events"][0].erase(field);
        expect_error(message.dump());
        message = original;
        message["events"][0][field] = 1;
        expect_error(message.dump());
    }
    for (const auto* field : {"side", "event_time", "price_level", "new_quantity"}) {
        auto message = original;
        message["events"][0]["updates"][1].erase(field);
        expect_error(message.dump());
        message = original;
        message["events"][0]["updates"][1][field] = 1.25;
        expect_error(message.dump());
    }
    for (const auto* field : {"type", "product_id"}) {
        auto message = original;
        message["events"][0][field] = "unknown";
        expect_error(message.dump());
    }
    for (const auto* field : {"price_level", "new_quantity"}) {
        for (const auto* value :
             {"-1", "", "1e3", "nan", "1.", ".1", " 1", "1.000000000", "9223372036854775808"}) {
            auto message = original;
            message["events"][0]["updates"][1][field] = value;
            expect_error(message.dump());
        }
    }
}

TEST_CASE("sequence numbers require unsigned integer range without rounding") {
    auto message = json::parse(fixture("l2_update"));
    for (const auto& value : {json{-1}, json{1.5}, json{1.0}, json{"42"}, json{true}}) {
        message["sequence_num"] = value[0];
        expect_error(message.dump());
    }
    for (const auto value : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
        message["sequence_num"] = value;
        const auto result = parse(message.dump());
        REQUIRE(result.status == ParseStatus::Parsed);
        CHECK(std::get<BookUpdate>(result.events[0]).sequence == value);
    }
    auto raw = message.dump();
    raw.replace(raw.find("18446744073709551615"), 20, "18446744073709551616");
    expect_error(raw);
}

TEST_CASE("UTC timestamp parser validates calendar precision and int64 boundaries") {
    auto message = json::parse(fixture("l2_update"));
    for (const auto* value :
         {"2026-02-29T00:00:00Z", "2024-04-31T00:00:00Z", "2026-01-01T24:00:00Z",
          "2026-01-01T00:60:00Z", "2026-01-01T00:00:60Z", "2026-01-01T00:00:00.Z",
          "2026-01-01T00:00:00.1234567890Z", "2026-01-01T00:00:00+00:00", "2026-01-01T00:00:00z",
          "0000-01-01T00:00:00Z", "2262-04-11T23:47:16.854775808Z",
          "1677-09-21T00:12:43.145224191Z", "bad"}) {
        message["timestamp"] = value;
        expect_error(message.dump());
    }
    for (const auto& [value, expected] : std::vector<std::pair<std::string, std::int64_t>>{
             {"1970-01-01T00:00:00Z", 0},
             {"1970-01-01T00:00:00.1Z", 100000000},
             {"1969-12-31T23:59:59.999999999Z", -1},
             {"2000-02-29T00:00:00Z", 951782400000000000},
             {"2262-04-11T23:47:16.854775807Z", std::numeric_limits<std::int64_t>::max()},
             {"1677-09-21T00:12:43.145224192Z", std::numeric_limits<std::int64_t>::min()}}) {
        message["timestamp"] = value;
        const auto result = parse(message.dump());
        REQUIRE(result.status == ParseStatus::Parsed);
        CHECK(std::get<BookUpdate>(result.events[0]).exchange_time.nanoseconds == expected);
    }
}

TEST_CASE("product configuration supplies precision and values remain exact beyond double") {
    CoinbaseL2Parser parser{CoinbaseSymbolMapper{
        CoinbaseSymbolMapper::Products{{"BTC-USD", {Instrument::BTC_USD, 0, 0}}}}};
    auto message = json::parse(fixture("l2_update"));
    message["events"][0]["updates"] = json::array({{{"side", "offer"},
                                                    {"event_time", "2026-09-06T20:30:01Z"},
                                                    {"price_level", "9007199254740993"},
                                                    {"new_quantity", "9223372036854775807"}}});
    const auto result =
        parser.parse(message.dump(), ReceiveWallTimestamp{1}, ReceiveMonotonicTimestamp{2});
    REQUIRE(result.status == ParseStatus::Parsed);
    const auto& level = std::get<BookUpdate>(result.events[0]).changes[0];
    CHECK(level.price.raw() == 9007199254740993LL);
    CHECK(level.quantity.raw() == std::numeric_limits<std::int64_t>::max());
    CHECK_FALSE(CoinbaseSymbolMapper{}.lookup("ETH-USD").has_value());
    CHECK_THROWS_AS(CoinbaseSymbolMapper(
                        CoinbaseSymbolMapper::Products{{"BTC-USD", {Instrument::BTC_USD, 19, 8}}}),
                    std::invalid_argument);
}
