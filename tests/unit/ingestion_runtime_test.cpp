#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "adapters/coinbase/coinbase_message_handler.hpp"
#include "benchmarks/benchmark_statistics.hpp"
#include "pipeline/joining_thread.hpp"
#include "pipeline/runtime_status.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <doctest.h>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
using namespace adapters::coinbase;
using Queue = recording::RawRecordingQueue;
using Push = Queue::PushResult;
static recording::RawEnvelopePtr envelope(std::uint64_t index, std::string payload) {
    return std::make_shared<const recording::RawEnvelope>(
        recording::RawEnvelope{index, core::Venue::Coinbase, 1, {0}, {0}, std::move(payload)});
}
TEST_CASE("bounded FIFO accounts bytes and drains after close") {
    Queue queue{{2, 5}};
    CHECK(queue.try_push(envelope(0, "abc")) == Push::Accepted);
    CHECK(queue.try_push(envelope(1, "de")) == Push::Accepted);
    CHECK(queue.payload_bytes() == 5);
    CHECK(queue.try_push(envelope(2, "")) == Push::Full);
    CHECK((*queue.wait_pop())->record_index == 0);
    CHECK(queue.payload_bytes() == 2);
    CHECK(queue.try_push(envelope(2, "abcd")) == Push::Full);
    CHECK(queue.try_push(envelope(2, "123456")) == Push::Full);
    CHECK(queue.try_push(envelope(2, "f")) == Push::Accepted);
    queue.close();
    queue.close();
    CHECK(queue.try_push(envelope(3, "")) == Push::Closed);
    CHECK((*queue.wait_pop())->record_index == 1);
    CHECK((*queue.wait_pop())->record_index == 2);
    CHECK_FALSE(queue.wait_pop());
    CHECK(queue.message_count() == 0);
    CHECK(queue.payload_bytes() == 0);
}
TEST_CASE("blocked consumer wakes on both push and close") {
    for (bool close : {false, true}) {
        Queue queue;
        std::promise<void> entered;
        std::promise<bool> consumed;
        auto future = consumed.get_future();
        pipeline::JoiningThread consumer([&] {
            entered.set_value();
            consumed.set_value(queue.wait_pop().has_value());
        });
        entered.get_future().wait();
        CHECK(future.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
        if (close)
            queue.close();
        else
            CHECK(queue.try_push(envelope(0, "a")) == Push::Accepted);
        CHECK(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        queue.close();
        consumer.join();
        CHECK(future.get() == !close);
    }
}
static auto now() {
    return core::ReceiveMonotonicTimestamp{std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count()};
}
static nlohmann::json fixture(const char* name) {
    std::ifstream in{std::string(FIXTURE_DIR) + "/" + name};
    nlohmann::json j;
    in >> j;
    return j;
}
struct HandlerHarness {
    Queue queue;
    core::OrderBook book{core::Venue::Coinbase, core::Instrument::BTC_USD};
    core::SequenceTracker sequence;
    FeedHealth health;
    int published{0};
    CoinbaseMessageHandler handler{book, sequence, health,
                                   [&](auto e) { return queue.try_push(e); },
                                   [&](const auto&, const auto&) { ++published; }};
    auto send(const nlohmann::json& j) {
        auto payload = j.dump();
        return handler.handle(std::move(payload), {1}, now());
    }
};
TEST_CASE("one latency per successful envelope, excluded outcomes and atomic multi-event apply") {
    HandlerHarness h;
    auto snapshot = fixture("l2_snapshot.json");
    auto first = h.send(snapshot);
    CHECK(first.outcome == MessageOutcome::BookUpdated);
    REQUIRE(first.latency);
    CHECK(first.latency->count() >= 0);
    CHECK(first.snapshots == 1);
    auto update = fixture("l2_update.json");
    auto second = h.send(update);
    CHECK(second.outcome == MessageOutcome::BookUpdated);
    REQUIRE(second.latency);
    CHECK(second.updates == 1);
    CHECK(h.book.last_sequence() == 43);
    auto duplicate = h.send(update);
    CHECK(duplicate.outcome == MessageOutcome::Duplicate);
    CHECK_FALSE(duplicate.latency);
    auto old = h.send(snapshot);
    CHECK(old.outcome == MessageOutcome::OutOfOrder);
    CHECK_FALSE(old.latency);
    auto heartbeat = h.send({{"channel", "heartbeats"},
                             {"sequence_num", 44},
                             {"events", nlohmann::json::array({nlohmann::json::object()})}});
    CHECK(heartbeat.outcome == MessageOutcome::Ignored);
    CHECK_FALSE(heartbeat.latency);
    update["sequence_num"] = 45;
    update["events"].push_back(update["events"][0]);
    update["events"][1]["updates"][0]["new_quantity"] = "7";
    auto multi = h.send(update);
    REQUIRE(multi.latency);
    CHECK(multi.canonical_event_count == 2);
    CHECK(h.book.best_bid()->quantity.raw() == 700000000);
    CHECK(h.book.last_sequence() == 45);
    update["sequence_num"] = 47;
    auto gap = h.send(update);
    CHECK(gap.outcome == MessageOutcome::SequenceGap);
    CHECK_FALSE(gap.latency);
    CHECK(h.book.state() == core::BookState::Invalid);
    h.sequence.reset();
    update["sequence_num"] = 48;
    auto awaiting = h.send(update);
    CHECK(awaiting.outcome == MessageOutcome::ApplyError);
    CHECK_FALSE(awaiting.latency);
    auto malformed = h.handler.handle("{", {0}, now());
    CHECK(malformed.outcome == MessageOutcome::ParseError);
    CHECK_FALSE(malformed.latency);
    CHECK(h.handler.progress.last_received_index == h.handler.progress.last_enqueued_index);
    CHECK(h.handler.progress.last_received_index == h.handler.progress.last_processed_index);
    CHECK(h.queue.message_count() == h.published);
}
TEST_CASE("queue rejection precedes parsing and invalidates an existing book") {
    HandlerHarness h;
    REQUIRE(h.send(fixture("l2_snapshot.json")).latency);
    h.queue.close();
    auto rejected = h.handler.handle("{ malformed", {0}, now());
    CHECK(rejected.outcome == MessageOutcome::RecordingRejected);
    CHECK_FALSE(rejected.latency);
    CHECK(h.book.state() == core::BookState::Invalid);
    CHECK(h.handler.progress.last_received_index == 1);
    CHECK(h.handler.progress.last_processed_index == 0);
    Queue full{{0, 0}};
    CoinbaseMessageHandler handler{h.book, h.sequence, h.health,
                                   [&](auto e) { return full.try_push(e); }};
    rejected = handler.handle("{", {0}, now());
    CHECK(rejected.error == "RAW_RECORDING_QUEUE_FULL");
    CHECK_FALSE(handler.progress.last_enqueued_index);
}
TEST_CASE("unpersisted tail uses last successful write, including no writes") {
    pipeline::RuntimeStatus s;
    CHECK(pipeline::unpersisted_processed_range(s).empty());
    s.progress.last_processed_index = 4817;
    s.progress.last_written_index = 4811;
    CHECK(pipeline::unpersisted_processed_range(s) == "unpersisted_processed_range=4812-4817");
    s.progress.last_written_index.reset();
    CHECK(pipeline::unpersisted_processed_range(s) == "unpersisted_processed_range=0-4817");
    s.progress.last_written_index = 4817;
    CHECK(pipeline::unpersisted_processed_range(s).empty());
}
TEST_CASE("disk preflight failure prevents writer readiness and closes queue") {
    Queue queue;
    pipeline::SharedRuntime shared;
    const auto path = std::filesystem::temp_directory_path() / "unopened-preflight.jsonl";
    pipeline::write_raw(queue, shared, path, std::numeric_limits<std::uintmax_t>::max());
    CHECK_FALSE(shared.writer_ready);
    CHECK(shared.writer_done);
    REQUIRE(shared.status.fatal_error);
    CHECK(*shared.status.fatal_error == "Insufficient disk space");
    CHECK(queue.try_push(envelope(0, "a")) == Push::Closed);
}
TEST_CASE("writer error posts failure to market context without mutating it on writer") {
    Queue queue;
    pipeline::SharedRuntime shared;
    boost::asio::io_context io;
    std::thread::id callback_thread;
    shared.post_recording_failure = [&](std::string) {
        boost::asio::post(io, [&] { callback_thread = std::this_thread::get_id(); });
    };
    pipeline::JoiningThread writer([&] { shared.recording_failed("injected write error"); });
    writer.join();
    CHECK(callback_thread == std::thread::id{});
    io.run();
    CHECK(callback_thread == std::this_thread::get_id());
    CHECK(shared.copy_status().runtime_state == pipeline::RuntimeState::StoppedRecordingError);
}
TEST_CASE("nearest-rank statistics have no timing assertions") {
    auto empty = benchmarks::statistics({});
    CHECK(empty.samples == 0);
    auto single = benchmarks::statistics({9});
    CHECK(single.p99_ns == 9);
    auto s = benchmarks::statistics({5, 1, 3, 4, 2});
    CHECK(s.min_ns == 1);
    CHECK(s.max_ns == 5);
    CHECK(s.mean_ns == 3);
    CHECK(s.p50_ns == 3);
    CHECK(s.p95_ns == 5);
    CHECK(s.p99_ns == 5);
}

TEST_CASE("asynchronous append failure preserves exact written position and processed tail") {
    auto pattern = (std::filesystem::temp_directory_path() / "writer-failure-XXXXXX").string();
    REQUIRE(mkdtemp(pattern.data()) != nullptr);
    auto path = std::filesystem::path(pattern) / "raw.jsonl";
    Queue queue;
    pipeline::SharedRuntime shared;
    shared.status.progress.last_processed_index = 2;
    for (std::uint64_t i = 0; i < 3; ++i)
        REQUIRE(queue.try_push(envelope(i, "{}")) == Push::Accepted);
    queue.close();
    pipeline::JoiningThread writer([&] {
        pipeline::write_raw(queue, shared, path, 0, [](auto& recorder, const auto& e) {
            if (e.record_index == 1)
                return false;
            return recorder.append(e.venue, e.connection_id, e.receive_wall_time,
                                   e.receive_monotonic_time, e.payload) &&
                   recorder.flush();
        });
    });
    writer.join();
    auto s = shared.copy_status();
    CHECK(s.runtime_state == pipeline::RuntimeState::StoppedRecordingError);
    CHECK(s.written_messages == 1);
    CHECK(s.progress.last_written_index == 0);
    CHECK(pipeline::unpersisted_processed_range(s) == "unpersisted_processed_range=1-2");
    REQUIRE(s.fatal_error);
    CHECK(s.fatal_error->find("index 1") != std::string::npos);
    CHECK(shared.writer_done);
    std::filesystem::remove_all(pattern);
}
