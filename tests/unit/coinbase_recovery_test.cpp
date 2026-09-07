#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/coinbase_connection_manager.hpp"
#include "recording/raw_event_reader.hpp"
#include "recording/raw_event_recorder.hpp"
#include <doctest.h>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace adapters::coinbase;
using State = core::BookState;
struct Harness {
    boost::asio::io_context io;
    std::int64_t time{0};
    CoinbaseWebSocketClient::MessageHandler message;
    CoinbaseWebSocketClient::ErrorHandler error;
    std::function<void()> connected, pending;
    std::vector<std::chrono::milliseconds> delays;
    std::vector<std::uint64_t> ids;
    bool sink_ok{true};
    int closes{0};
    CoinbaseConnectionManager::Sink sink;
    CoinbaseConnectionManager manager{io,
                                      [this](auto id, auto raw, auto wall, auto mono) {
                                          ids.push_back(id);
                                          return sink_ok && (!sink || sink(id, raw, wall, mono));
                                      },
                                      [this](auto m, auto e, auto c) {
                                          message = std::move(m);
                                          error = std::move(e);
                                          connected = std::move(c);
                                      },
                                      [this] { ++closes; },
                                      [this] { return core::ReceiveMonotonicTimestamp{time}; },
                                      {},
                                      [this](auto delay, auto resume) {
                                          delays.push_back(delay);
                                          pending = std::move(resume);
                                      }};
    Harness() { manager.start(); }
    void reconnect() {
        auto resume = std::move(pending);
        REQUIRE(resume);
        resume();
        connected();
    }
    bool send(std::string raw) { return message(raw, {123}, {time}); }
};
static std::string l2(std::uint64_t seq, bool snapshot, const char* price = "100") {
    std::ifstream in{std::string{FIXTURE_DIR} + "/l2_snapshot.json"};
    nlohmann::json j;
    in >> j;
    j["sequence_num"] = seq;
    j["events"][0]["type"] = snapshot ? "snapshot" : "update";
    auto level = j["events"][0]["updates"][0];
    level["price_level"] = price;
    j["events"][0]["updates"] = nlohmann::json::array({level});
    return j.dump();
}
TEST_CASE("gap and premature recovery update fail closed; fresh snapshot replaces old levels") {
    Harness h;
    auto path = std::filesystem::temp_directory_path() / "coinbase_recovery_test.jsonl";
    std::filesystem::remove(path);
    recording::RawEventRecorder recorder{path};
    h.sink = [&](auto id, auto raw, auto wall, auto mono) {
        return recorder.append(core::Venue::Coinbase, id, wall, mono, raw);
    };
    h.connected();
    REQUIRE(h.send(l2(100, true)));
    REQUIRE(h.send(l2(101, false, "101")));
    const auto old = h.manager.book().bids();
    auto obsolete_message = h.message;
    auto obsolete_error = h.error;
    CHECK_FALSE(h.send(l2(103, false, "103")));
    CHECK(h.manager.book().state() == State::Invalid);
    CHECK(h.manager.book().bids() == old);
    CHECK_FALSE(h.manager.book().best_bid());
    CHECK_FALSE(h.manager.book().best_ask());
    CHECK_FALSE(h.manager.book().has_two_sided_market());
    obsolete_error("another failure");
    h.manager.check_health();
    CHECK(h.delays.size() == 1);
    h.time = 1000000000;
    h.reconnect();
    CHECK(h.manager.connection_id() == 2);
    CHECK_FALSE(obsolete_message(l2(104, true), {0}, {h.time}));
    CHECK_FALSE(h.send(l2(500, false)));
    CHECK(h.manager.book().bids() == old);
    CHECK(h.manager.metrics().updates_before_snapshot == 1);
    CHECK(h.delays.size() == 2);
    CHECK(h.delays.back().count() == 500);
    h.time = 2000000000;
    h.reconnect();
    REQUIRE(h.send(l2(501, true, "200")));
    CHECK(h.manager.book().state() == State::Valid);
    CHECK(h.manager.book().bids().size() == 1);
    REQUIRE(h.manager.book().best_bid());
    CHECK(h.manager.book().best_bid()->price.raw() == 20000);
    REQUIRE(h.send(l2(502, false, "201")));
    CHECK(h.manager.health().sequence_gaps == 1);
    CHECK(h.manager.metrics().recovery_successes == 1);
    CHECK(h.manager.metrics().recovery_duration.count() == 2000000000);
    CHECK(h.manager.metrics().snapshots_received == 2);
    CHECK(h.manager.metrics().updates_received == 2);
    REQUIRE(recorder.close());
    recording::RawEventReader reader{path};
    std::vector<std::uint64_t> recorded;
    while (auto r = reader.next())
        recorded.push_back(r->connection_id);
    CHECK_FALSE(reader.has_error());
    CHECK(recorded == std::vector<std::uint64_t>{1, 1, 1, 2, 3, 3});
    h.manager.force_disconnect_for_test();
    CHECK(h.delays.back().count() == 250);
    h.manager.stop();
    auto started = h.manager.metrics().connections_started;
    h.pending();
    CHECK(h.manager.metrics().connections_started == started);
    std::filesystem::remove(path);
}
TEST_CASE("duplicates, old sequences and heartbeat channel do not mutate L2") {
    Harness h;
    h.connected();
    REQUIRE(h.send(l2(100, true)));
    auto old = h.manager.book().bids();
    REQUIRE(h.send(l2(100, false, "999")));
    REQUIRE(h.send(l2(99, false, "999")));
    REQUIRE(h.send(
        R"({"channel":"heartbeats","sequence_num":101,"events":[{"heartbeat_counter":"2"}]})"));
    CHECK(h.manager.book().bids() == old);
    CHECK(h.manager.sequence() == 101);
    CHECK(h.manager.health().duplicate_sequences == 1);
    CHECK(h.manager.health().out_of_order_sequences == 1);
    CHECK(h.manager.health().heartbeat_count == 1);
    CHECK(h.send(l2(102, false)));
}
TEST_CASE("failed handshakes retain backoff and shutdown cancels callback") {
    Harness h;
    h.error("DNS failure");
    CHECK(h.delays.back().count() == 250);
    h.reconnect();
    h.error("read failure before snapshot");
    CHECK(h.delays.back().count() == 500);
    h.manager.stop();
    auto started = h.manager.metrics().connections_started;
    h.pending();
    CHECK(h.manager.metrics().connections_started == started);
}
TEST_CASE("timeouts recover once and quiet L2 remains healthy with heartbeats") {
    Harness h;
    h.connected();
    REQUIRE(h.send(l2(100, true)));
    h.time = 6000000000;
    REQUIRE(h.send(R"({"channel":"heartbeats","events":[{}]})"));
    h.manager.check_health();
    CHECK(h.manager.book().state() == State::Valid);
    h.time = 12000000000;
    REQUIRE(h.send(R"({"type":"subscriptions"})"));
    h.manager.check_health();
    CHECK(h.manager.book().state() == State::Stale);
    CHECK(h.manager.health().heartbeat_timeouts == 1);
    h.manager.check_health();
    CHECK(h.delays.size() == 1);
    h.reconnect();
    h.time = 18000000000;
    h.manager.check_health();
    CHECK(h.manager.metrics().last_failure_reason == "No Coinbase messages");
}
TEST_CASE("malformed L2, unsupported product, explicit errors, recorder and application failures") {
    for (int failure = 0; failure < 6; ++failure) {
        Harness h;
        h.connected();
        REQUIRE(h.send(l2(100, true)));
        auto j = nlohmann::json::parse(l2(101, false));
        if (failure == 0)
            j.erase("sequence_num");
        if (failure == 1)
            j["events"][0]["product_id"] = "ETH-USD";
        if (failure == 2)
            j = {{"type", "error"}, {"message", "denied"}};
        if (failure == 3)
            h.sink_ok = false;
        if (failure == 4) {
            j["events"][0]["type"] = "snapshot";
            j["events"][0]["updates"].push_back(j["events"][0]["updates"][0]);
        }
        if (failure == 5)
            j["events"][0]["updates"][0]["price_level"] = "bad";
        CHECK_FALSE(h.send(j.dump()));
        CHECK(h.manager.book().state() == State::Invalid);
        CHECK_FALSE(h.manager.book().best_bid());
        CHECK(h.delays.size() == 1);
    }
}

TEST_CASE("empty L2 envelopes advance continuity but cannot establish a snapshot") {
    Harness h;
    h.connected();
    auto empty = nlohmann::json::parse(l2(100, true));
    empty["events"] = nlohmann::json::array();
    CHECK_FALSE(h.send(empty.dump()));
    h.reconnect();
    REQUIRE(h.send(l2(100, true)));
    empty["sequence_num"] = 101;
    REQUIRE(h.send(empty.dump()));
    CHECK(h.manager.sequence() == 101);
    REQUIRE(h.send(l2(102, false)));
}
TEST_CASE("stalled connection attempt has a monotonic deadline") {
    Harness h;
    h.time = 31000000000LL;
    h.manager.check_health();
    CHECK(h.delays.size() == 1);
    CHECK(h.manager.metrics().last_failure_reason == "Coinbase connection timeout");
}
TEST_CASE("forced disconnect requires snapshot and increases successful session ID") {
    Harness h;
    h.connected();
    REQUIRE(h.send(l2(42, true)));
    h.manager.force_disconnect_for_test();
    CHECK(h.manager.book().state() == State::Disconnected);
    CHECK_FALSE(h.manager.book().best_bid());
    h.reconnect();
    CHECK(h.manager.connection_id() == 2);
    CHECK(h.manager.book().state() == State::Initializing);
    REQUIRE(h.send(l2(0, true, "250")));
    CHECK(h.manager.book().best_bid()->price.raw() == 25000);
    CHECK(h.manager.metrics().recovery_successes == 1);
}

TEST_CASE("all channels share continuity without changing the book") {
    Harness h;
    h.connected();
    REQUIRE(h.send(R"({"channel":"subscriptions","sequence_num":0})"));
    REQUIRE(h.send(R"({"channel":"heartbeats","sequence_num":1,"events":[{}]})"));
    CHECK(h.manager.book().state() == State::Initializing);
    CHECK_FALSE(h.manager.book().best_bid());
    REQUIRE(h.send(l2(2, true)));
    const auto levels = h.manager.book().bids();
    REQUIRE(h.send(R"({"channel":"subscriptions","sequence_num":3})"));
    REQUIRE(h.send(R"({"channel":"future","sequence_num":4})"));
    REQUIRE(h.send(R"({"channel":"heartbeats","sequence_num":5,"events":[{}]})"));
    CHECK(h.manager.book().bids() == levels);
    CHECK(h.manager.book().last_sequence() == 2);
    CHECK(h.manager.sequence() == 5);
    REQUIRE(h.send(l2(6, false)));
    CHECK(h.delays.empty());
    // Duplicate and old non-L2 envelopes cannot refresh heartbeat health.
    const auto heartbeat_time = h.manager.health().last_heartbeat;
    h.time = 1000000000;
    REQUIRE(h.send(R"({"channel":"heartbeats","sequence_num":6,"events":[{}]})"));
    REQUIRE(h.send(R"({"channel":"heartbeats","sequence_num":5,"events":[{}]})"));
    CHECK(h.manager.health().last_heartbeat == heartbeat_time);
    CHECK(h.manager.health().duplicate_sequences == 1);
    CHECK(h.manager.health().out_of_order_sequences == 1);
    CHECK_FALSE(h.send(R"({"channel":"heartbeats","sequence_num":8,"events":[{}]})"));
    CHECK(h.manager.book().state() == State::Invalid);
    CHECK(h.manager.health().sequence_gaps == 1);
    CHECK(h.delays.size() == 1);
}
TEST_CASE("non-L2 baseline does not permit updates before snapshot") {
    Harness h;
    h.connected();
    REQUIRE(h.send(R"({"channel":"subscriptions","sequence_num":100})"));
    CHECK_FALSE(h.send(l2(101, false)));
    CHECK(h.manager.metrics().updates_before_snapshot == 1);
    h.reconnect();
    REQUIRE(h.send(R"({"channel":"heartbeats","sequence_num":0,"events":[{}]})"));
    REQUIRE(h.send(l2(1, true)));
    CHECK(h.manager.book().state() == State::Valid);
}
TEST_CASE("malformed sequence on any channel fails closed") {
    for (const auto* channel : {"heartbeats", "subscriptions", "future"}) {
        Harness h;
        h.connected();
        REQUIRE(h.send(l2(0, true)));
        nlohmann::json j{{"channel", channel}, {"sequence_num", -1}};
        CHECK_FALSE(h.send(j.dump()));
        CHECK(h.manager.book().state() == State::Invalid);
        CHECK(h.delays.size() == 1);
    }
}
