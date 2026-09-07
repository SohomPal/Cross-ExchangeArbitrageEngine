#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/coinbase_message_pipeline.hpp"
#include "adapters/coinbase/coinbase_subscription.hpp"
#include "recording/raw_event_reader.hpp"
#include "recording/raw_event_recorder.hpp"
#include <doctest.h>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace adapters::coinbase;
static std::string fixture(const char* name) {
    std::ifstream in{std::filesystem::path{FIXTURE_DIR} / name};
    return {std::istreambuf_iterator<char>{in}, {}};
}
TEST_CASE("subscriptions contain the public channel requests") {
    CHECK(nlohmann::json::parse(subscriptions[0]) ==
          nlohmann::json::parse(
              R"({"type":"subscribe","channel":"level2","product_ids":["BTC-USD"]})"));
    CHECK(nlohmann::json::parse(subscriptions[1]) ==
          nlohmann::json::parse(R"({"type":"subscribe","channel":"heartbeats"})"));
}
TEST_CASE("record before parsing, close flushes, replay reproduces all levels and sequence") {
    const auto path = std::filesystem::temp_directory_path() / "coinbase_pipeline_test.jsonl";
    std::filesystem::remove(path);
    recording::RawEventRecorder recorder{path};
    CoinbaseMessagePipeline* active = nullptr;
    CoinbaseMessagePipeline live{[&](auto raw, auto wall, auto mono) {
        // Snapshot has not reached the book when the sink is called.
        if (recorder.next_record_index() == 0)
            CHECK(active->book.state() == core::BookState::Initializing);
        return recorder.append(core::Venue::Coinbase, 1, wall, mono, raw);
    }};
    active = &live;
    REQUIRE(live.process(fixture("l2_snapshot.json"), {1}, {2}));
    CHECK(live.book.state() == core::BookState::Valid);
    REQUIRE(live.process(fixture("l2_update.json"), {3}, {4}));
    CHECK(live.book.bids().at(core::parse_price("62143.27", 2)) == core::parse_quantity("0.5", 8));
    const auto bids = live.book.bids();
    const auto asks = live.book.asks();
    const auto sequence = live.book.last_sequence();
    for (const auto& raw : {R"({"channel":"heartbeats"})", R"({"type":"subscriptions"})",
                            R"({"type":"future_type"})"})
        REQUIRE(live.process(raw, {5}, {6}));
    CHECK(live.book.bids() == bids);
    CHECK(live.book.asks() == asks);
    CHECK(live.book.last_sequence() == sequence);
    REQUIRE(recorder.close());
    CHECK(live.raw_messages == 5);
    recording::RawEventReader reader{path};
    CoinbaseMessagePipeline replay{[](auto, auto, auto) { return true; }};
    while (auto record = reader.next())
        REQUIRE(replay.process(record->payload, record->receive_wall_time,
                               record->receive_monotonic_time));
    CHECK_FALSE(reader.has_error());
    CHECK(replay.raw_messages == live.raw_messages);
    CHECK(replay.book.bids() == bids);
    CHECK(replay.book.asks() == asks);
    CHECK(replay.book.last_sequence() == sequence);
    CHECK(replay.book.state() == live.book.state());
    std::filesystem::remove(path);
}
TEST_CASE("failures invalidate without discarding diagnostic levels and stop processing") {
    for (const auto& raw : {std::string{"{"}, std::string{R"({"type":"error","message":"denied"})"},
                            fixture("malformed_l2.json")}) {
        int recorded = 0;
        CoinbaseMessagePipeline pipeline{[&](auto, auto, auto) {
            ++recorded;
            return true;
        }};
        REQUIRE(pipeline.process(fixture("l2_snapshot.json"), {1}, {2}));
        auto bids = pipeline.book.bids();
        CHECK_FALSE(pipeline.process(raw, {3}, {4}));
        CHECK(recorded == 2);
        CHECK(pipeline.parse_errors == 1);
        CHECK(pipeline.book.state() == core::BookState::Invalid);
        CHECK(pipeline.book.bids() == bids);
        CHECK_FALSE(pipeline.process(fixture("l2_update.json"), {5}, {6}));
        CHECK(recorded == 2);
    }
}
TEST_CASE("recorder failure prevents parsing and update before snapshot fails application") {
    CoinbaseMessagePipeline failed_sink{[](auto, auto, auto) { return false; }};
    CHECK_FALSE(failed_sink.process("{", {1}, {2}));
    CHECK(failed_sink.parse_errors == 0);
    CHECK(failed_sink.raw_messages == 0);
    CHECK(failed_sink.book.state() == core::BookState::Invalid);
    CoinbaseMessagePipeline early_update{[](auto, auto, auto) { return true; }};
    CHECK_FALSE(early_update.process(fixture("l2_update.json"), {1}, {2}));
    CHECK(early_update.book.state() == core::BookState::Invalid);
}
