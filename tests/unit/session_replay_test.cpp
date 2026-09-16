#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "recording/raw_event_reader.hpp"
#include "session/session.hpp"
#include <fstream>
#include <unistd.h>
using nlohmann::json;
namespace {
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "session-test-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back(0);
        auto result = mkdtemp(buffer.data());
        if (!result)
            throw std::runtime_error("temporary directory failed");
        path = result;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
json read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return json::parse(in);
}
void write(const std::filesystem::path& path, const json& value) {
    std::ofstream out(path);
    out << value.dump() << '\n';
}
pipeline::RuntimeStatus record(sessions::Capture& capture) {
    recording::RawRecordingQueue queue;
    pipeline::SharedRuntime runtime;
    recording::RawEventReader reader(std::filesystem::path(FIXTURE_DIR) / "benchmark.jsonl");
    while (auto e = reader.next()) {
        auto index = e->record_index;
        REQUIRE(queue.try_push(std::make_shared<const recording::RawEnvelope>(*e)) ==
                recording::RawRecordingQueue::PushResult::Accepted);
        ++runtime.status.received_messages;
        ++runtime.status.enqueued_messages;
        ++runtime.status.processed_messages;
        runtime.status.progress.last_received_index = index;
        runtime.status.progress.last_enqueued_index = index;
        runtime.status.progress.last_processed_index = index;
    }
    REQUIRE_FALSE(reader.has_error());
    queue.close();
    pipeline::write_raw(queue, runtime, capture.raw_path(), 0);
    REQUIRE(runtime.writer_done);
    REQUIRE_FALSE(runtime.status.fatal_error);
    runtime.status.runtime_state = pipeline::RuntimeState::Stopped;
    return runtime.status;
}
} // namespace
TEST_CASE("capture finalizes only after shutdown and publishes immutable manifests") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    CHECK(std::filesystem::exists(source / "manifest.inprogress.json"));
    CHECK_FALSE(std::filesystem::exists(source / "manifest.json"));
    CHECK_THROWS(capture.finalize(pipeline::RuntimeStatus{}));
    auto status = record(capture);
    CHECK_FALSE(std::filesystem::exists(source / "manifest.json"));
    capture.finalize(status);
    auto manifest = read(source / "manifest.json");
    CHECK(manifest["status"] == "complete");
    CHECK(manifest["raw_files"][0]["sha256"] == sessions::file_sha256(capture.raw_path()));
    CHECK(manifest["raw_files"][0]["bytes"] == std::filesystem::file_size(capture.raw_path()));
    CHECK(manifest["raw_files"][0]["records"] == 12);
    CHECK_FALSE(std::filesystem::exists(source / "manifest.inprogress.json"));
    CHECK_THROWS(capture.finalize(status));
    CHECK(read(source / "manifest.json") == manifest);
    CHECK_THROWS(sessions::Capture(source));
}
TEST_CASE("same recorded data produces identical functional results with and without full output") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    capture.finalize(record(capture));
    sessions::replay(source, dir.path / "one.json", false, true);
    sessions::replay(source, dir.path / "two.json", false, true);
    CHECK(read(dir.path / "one.json") == read(dir.path / "two.json"));
    sessions::replay(source, dir.path / "compact.json");
    auto full = read(dir.path / "one.json"), compact = read(dir.path / "compact.json");
    CHECK(full["deterministic_result_hash"] == compact["deterministic_result_hash"]);
    CHECK_FALSE(compact.contains("final_book"));
    CHECK(full["final_sequence"] == 2);
    CHECK(full["outcomes"]["BookUpdated"] == 4);
    CHECK(full["sequence_gaps"] == 1);
    CHECK(full["connection_boundaries"] == 1);
    CHECK(full["final_best_bid"] == 6214327);
    CHECK(full["final_best_ask"] == 6214450);
    CHECK_THROWS(sessions::replay(source, dir.path / "one.json"));
}
TEST_CASE("source validation fails closed before publishing a result") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    capture.finalize(record(capture));
    auto original = read(source / "manifest.json");
    const auto output = dir.path / "result.json";
    for (auto mutation : {"version", "checksum", "bytes", "count", "incomplete", "precision",
                          "path", "index_bounds", "lifecycle"}) {
        auto manifest = original;
        if (std::string(mutation) == "version")
            manifest["format_version"] = 2;
        if (std::string(mutation) == "checksum")
            manifest["raw_files"][0]["sha256"] = "bad";
        if (std::string(mutation) == "bytes")
            manifest["raw_files"][0]["bytes"] = 1;
        if (std::string(mutation) == "count")
            manifest["raw_files"][0]["records"] = 13;
        if (std::string(mutation) == "incomplete")
            manifest["status"] = "recording_failure";
        if (std::string(mutation) == "precision")
            manifest["product_metadata"]["coinbase:BTC-USD"]["price_scale"] = 3;
        if (std::string(mutation) == "path")
            manifest["raw_files"][0]["path"] = "../../elsewhere";
        if (std::string(mutation) == "index_bounds")
            manifest["raw_files"][0]["last_record_index"] = 12;
        if (std::string(mutation) == "lifecycle")
            manifest["lifecycle_events"] = json::array(
                {{{"before_record_index", 13}, {"state", "INVALID"}, {"reason", "bad"}}});
        write(source / "manifest.json", manifest);
        CHECK_THROWS(sessions::replay(source, output));
        CHECK_FALSE(std::filesystem::exists(output));
    }
    write(source / "manifest.json", original);
    std::filesystem::remove(capture.raw_path());
    CHECK_THROWS(sessions::replay(source, output));
}
TEST_CASE("diagnostic incomplete replay is visibly marked and does not bypass integrity checks") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    auto status = record(capture);
    status.runtime_state = pipeline::RuntimeState::StoppedRecordingError;
    status.fatal_error = "injected recording failure";
    ++status.received_messages;
    status.progress.last_received_index = 12;
    status.progress.last_processed_index = 12;
    capture.finalize(status);
    auto manifest = read(source / "manifest.json");
    CHECK(manifest["status"] == "recording_failure");
    CHECK(manifest["unpersisted_records"] == 1);
    CHECK(manifest["unpersisted_processed_tail"] == true);
    CHECK_THROWS(sessions::replay(source, dir.path / "blocked.json"));
    sessions::replay(source, dir.path / "diagnostic.json", true);
    CHECK(read(dir.path / "diagnostic.json")["complete_source"] == false);
    CHECK(read(dir.path / "diagnostic.json")["research_valid"] == false);
    std::ofstream out(capture.raw_path(), std::ios::app);
    out << "\n";
    out.close();
    CHECK_THROWS(sessions::replay(source, dir.path / "corrupt.json", true));
}
TEST_CASE("raw indexes, final newline and checksum are verified independently") {
    for (auto mutation :
         {"duplicate", "missing", "decreasing", "newline", "truncated", "changed", "type"}) {
        Directory dir;
        auto source = dir.path / "capture";
        sessions::Capture capture(source);
        capture.finalize(record(capture));
        auto manifest = read(source / "manifest.json");
        std::ifstream in(capture.raw_path());
        std::vector<json> lines;
        std::string line;
        while (std::getline(in, line))
            lines.push_back(json::parse(line));
        in.close();
        if (std::string(mutation) == "duplicate")
            lines[1]["record_index"] = 0;
        if (std::string(mutation) == "missing")
            lines[1].erase("record_index");
        if (std::string(mutation) == "decreasing")
            std::swap(lines[1], lines[2]);
        if (std::string(mutation) == "type")
            lines[1]["record_index"] = 1.5;
        if (std::string(mutation) == "changed")
            lines[0]["receive_wall_ns"] = 124;
        std::string bytes;
        for (const auto& entry : lines)
            bytes += entry.dump() + "\n";
        if (std::string(mutation) == "newline")
            bytes.pop_back();
        if (std::string(mutation) == "truncated")
            bytes.resize(bytes.size() - 10);
        std::ofstream out(capture.raw_path());
        out << bytes;
        out.close();
        if (std::string(mutation) != "changed") {
            manifest["raw_files"][0]["sha256"] = sessions::file_sha256(capture.raw_path());
            manifest["raw_files"][0]["bytes"] = bytes.size();
        }
        write(source / "manifest.json", manifest);
        CHECK_THROWS(sessions::replay(source, dir.path / "result.json"));
    }
}
TEST_CASE("lifecycle events reproduce disconnection after the last raw record") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    auto status = record(capture);
    status.lifecycle_events = {{0, core::BookState::Resyncing, "resynchronization"},
                               {0, core::BookState::Initializing, "new_connection"},
                               {12, core::BookState::Disconnected, "requested_shutdown"}};
    capture.finalize(status);
    sessions::replay(source, dir.path / "result.json");
    auto result = read(dir.path / "result.json");
    CHECK(result["final_book_state"] == "DISCONNECTED");
    CHECK(result["final_sequence"].is_null());
    CHECK(result["state_transitions"].back()["reason"] == "requested_shutdown");
}
TEST_CASE("a connection boundary rejects updates until a fresh snapshot") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    recording::RawEventReader fixture(std::filesystem::path(FIXTURE_DIR) / "benchmark.jsonl");
    auto snapshot = fixture.next();
    auto update = fixture.next();
    REQUIRE(snapshot);
    REQUIRE(update);
    recording::RawEventRecorder recorder(capture.raw_path());
    for (int i = 0; i < 5; ++i) {
        auto payload = json::parse((i == 0 || i == 3) ? snapshot->payload : update->payload);
        payload["sequence_num"] = i < 2 ? 42 + i : i - 2;
        REQUIRE(recorder.append(core::Venue::Coinbase, i < 2 ? 1 : 2, {5 - i}, {i % 2},
                                payload.dump()));
    }
    REQUIRE(recorder.close());
    pipeline::RuntimeStatus status;
    status.runtime_state = pipeline::RuntimeState::Stopped;
    status.received_messages = status.enqueued_messages = status.written_messages =
        status.processed_messages = 5;
    status.progress.last_received_index = status.progress.last_enqueued_index =
        status.progress.last_written_index = status.progress.last_processed_index = 4;
    capture.finalize(status);
    sessions::replay(source, dir.path / "result.json", false, true);
    auto result = read(dir.path / "result.json");
    CHECK(result["outcomes"]["ApplyError"] == 1);
    CHECK(result["snapshots"] == 2);
    CHECK(result["updates"] == 2);
    CHECK(result["final_book_state"] == "VALID");
    CHECK(result["final_sequence"] == 2);
    CHECK(result["connection_boundaries"] == 1);
    bool rejected = false, recovered = false;
    for (const auto& transition : result["state_transitions"]) {
        if (transition["record_index"] == 2 && transition["to"] == "INVALID")
            rejected = true;
        if (transition["record_index"] == 3 && transition["to"] == "VALID")
            recovered = true;
    }
    CHECK(rejected);
    CHECK(recovered);
}
TEST_CASE("an abandoned capture remains in progress and diagnostic replay uses stored timestamps") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    record(capture);
    CHECK_FALSE(std::filesystem::exists(source / "manifest.json"));
    CHECK_THROWS(sessions::replay(source, dir.path / "blocked.json"));
    sessions::replay(source, dir.path / "diagnostic.json", true);
    CHECK(read(dir.path / "diagnostic.json")["complete_source"] == false);
    core::ReplayClock clock;
    clock.set({123}, {-456});
    CHECK(clock.wall_now().nanoseconds == 123);
    CHECK(clock.monotonic_now().nanoseconds == -456);
}
TEST_CASE("symlinks cannot escape the session directory") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    capture.finalize(record(capture));
    auto outside = dir.path / "outside.jsonl";
    std::filesystem::rename(capture.raw_path(), outside);
    std::filesystem::create_symlink(outside, capture.raw_path());
    CHECK_THROWS(sessions::replay(source, dir.path / "result.json"));
}
TEST_CASE("live manager and replay agree across recovery and terminal shutdown") {
    Directory dir;
    auto source = dir.path / "capture";
    sessions::Capture capture(source);
    recording::RawRecordingQueue queue;
    pipeline::SharedRuntime runtime;
    boost::asio::io_context io;
    adapters::coinbase::CoinbaseWebSocketClient::MessageHandler message;
    std::function<void()> connected, reconnect;
    json live_outcomes = json::object();
    adapters::coinbase::CoinbaseConnectionManager manager(
        io, {},
        [&](auto on_message, auto, auto on_connected) {
            message = on_message;
            connected = on_connected;
        },
        [] {}, [] { return core::ReceiveMonotonicTimestamp{0}; }, {},
        [&](auto, auto resume) { reconnect = std::move(resume); },
        [&](auto e) { return queue.try_push(std::move(e)); },
        [&](const auto& result, const auto& progress) {
            runtime.status.progress = progress;
            ++runtime.status.received_messages;
            ++runtime.status.enqueued_messages;
            ++runtime.status.processed_messages;
            auto name = adapters::coinbase::outcome_name(result.outcome);
            live_outcomes[name] = live_outcomes.value(name, 0) + 1;
        });
    manager.observe_lifecycle = [&](const auto& e) {
        runtime.status.lifecycle_events.push_back(e);
    };
    recording::RawEventReader fixture(std::filesystem::path(FIXTURE_DIR) / "benchmark.jsonl");
    auto snapshot = fixture.next(), update = fixture.next();
    REQUIRE(snapshot);
    REQUIRE(update);
    manager.start();
    connected();
    REQUIRE(message(snapshot->payload, {123}, {0}));
    REQUIRE(message(update->payload, {123}, {0}));
    manager.force_disconnect_for_test();
    reconnect();
    connected();
    REQUIRE_FALSE(message(update->payload, {123}, {0})); // Abandon premature-update connection.
    reconnect();
    connected();
    REQUIRE(message(snapshot->payload, {123}, {0}));
    REQUIRE(message(update->payload, {123}, {0}));
    manager.stop();
    queue.close();
    pipeline::write_raw(queue, runtime, capture.raw_path(), 0);
    REQUIRE(runtime.writer_done);
    runtime.status.runtime_state = pipeline::RuntimeState::Stopped;
    capture.finalize(runtime.status);
    sessions::replay(source, dir.path / "replayed.json", false, true);
    auto result = read(dir.path / "replayed.json");
    for (auto it = live_outcomes.begin(); it != live_outcomes.end(); ++it)
        CHECK(result["outcomes"][it.key()] == it.value());
    CHECK(result["final_book_state"] == "DISCONNECTED");
    CHECK(result["final_sequence"].is_null());
    CHECK(result["final_bid_levels"] == manager.book().bids().size());
    CHECK(result["final_ask_levels"] == manager.book().asks().size());
    json bids = json::array(), asks = json::array();
    for (auto [p, q] : manager.book().bids())
        bids.push_back({p.raw(), q.raw()});
    for (auto [p, q] : manager.book().asks())
        asks.push_back({p.raw(), q.raw()});
    CHECK(result["final_book"]["bids"] == bids);
    CHECK(result["final_book"]["asks"] == asks);
}
