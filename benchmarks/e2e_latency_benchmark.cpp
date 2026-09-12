#include "adapters/coinbase/coinbase_message_handler.hpp"
#include "benchmarks/benchmark_statistics.hpp"
#include "benchmarks/latency_sample.hpp"
#include "pipeline/joining_thread.hpp"
#include "pipeline/runtime_status.hpp"
#include "recording/raw_event_reader.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <sstream>
#include <thread>
#include <unistd.h>
using nlohmann::json;
using namespace adapters::coinbase;
namespace {
std::string sha256(const std::string& bytes) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    std::ostringstream out;
    for (auto byte : digest)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    return out.str();
}
std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    std::string bytes{std::istreambuf_iterator<char>{in}, {}};
    if (in.bad())
        throw std::runtime_error("source read failed");
    return bytes;
}
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "e2e-benchmark-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            throw std::runtime_error("cannot create temporary directory");
        path = pattern;
    }
    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
json stats_json(const std::vector<std::int64_t>& values) {
    const auto s = benchmarks::statistics(values);
    return {{"samples", s.samples}, {"min_ns", s.min_ns}, {"mean_ns", s.mean_ns},
            {"p50_ns", s.p50_ns},   {"p95_ns", s.p95_ns}, {"p99_ns", s.p99_ns},
            {"max_ns", s.max_ns}};
}
template <class T> json optional_json(const std::optional<T>& value) {
    return value ? json(*value) : json(nullptr);
}
json trial(const std::vector<recording::RawMessage>& data, recording::RawQueueConfig config) {
    TemporaryDirectory directory;
    auto output = directory.path / "raw.jsonl";
    recording::RawRecordingQueue queue{config};
    pipeline::SharedRuntime shared;
    json result;
    std::exception_ptr failure;
    pipeline::JoiningThread writer([&] { pipeline::write_raw(queue, shared, output); });
    try {
        pipeline::JoiningThread market([&] {
            try {
                {
                    std::unique_lock lock(shared.mutex);
                    shared.changed.wait(lock,
                                        [&] { return shared.writer_ready || shared.writer_done; });
                    if (shared.status.fatal_error)
                        throw std::runtime_error(*shared.status.fatal_error);
                }
                core::OrderBook book{core::Venue::Coinbase, core::Instrument::BTC_USD};
                core::SequenceTracker sequence;
                FeedHealth health;
                CoinbaseMessageHandler handler{
                    book, sequence, health, [&](auto e) { return queue.try_push(std::move(e)); },
                    [&](const ProcessResult& processed, const RuntimeProgress& progress) {
                        std::lock_guard lock(shared.mutex);
                        auto& s = shared.status;
                        ++s.received_messages;
                        if (processed.outcome != MessageOutcome::RecordingRejected) {
                            ++s.enqueued_messages;
                            ++s.processed_messages;
                        }
                        s.progress.last_received_index = progress.last_received_index;
                        s.progress.last_enqueued_index = progress.last_enqueued_index;
                        s.progress.last_processed_index = progress.last_processed_index;
                        s.book_state = book.state();
                        s.last_sequence = sequence.last();
                        auto bid = book.best_bid(), ask = book.best_ask();
                        s.best_bid = bid ? std::optional{bid->price} : std::nullopt;
                        s.best_ask = ask ? std::optional{ask->price} : std::nullopt;
                    }};
                std::vector<std::int64_t> snapshots, updates, all;
                snapshots.reserve(data.size());
                updates.reserve(data.size());
                all.reserve(data.size());
                json outcomes = json::object();
                for (int i = 0; i <= static_cast<int>(MessageOutcome::ApplyError); ++i)
                    outcomes[outcome_name(static_cast<MessageOutcome>(i))] = 0;
                std::optional<std::uint64_t> connection;
                auto start = std::chrono::steady_clock::now();
                for (const auto& raw : data) {
                    if (!connection || *connection != raw.connection_id) {
                        connection = raw.connection_id;
                        handler.connection_id = *connection;
                        sequence.reset();
                        book.mark_initializing();
                        health = {};
                    }
                    // Dataset copying is outside the measured interval; ownership is then moved.
                    std::string payload = raw.payload;
                    const core::ReceiveMonotonicTimestamp mono{
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count()};
                    const core::ReceiveWallTimestamp wall{
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count()};
                    auto processed = handler.handle(std::move(payload), wall, mono);
                    auto name = outcome_name(processed.outcome);
                    outcomes[name] = outcomes[name].get<std::uint64_t>() + 1;
                    if (processed.outcome == MessageOutcome::RecordingRejected)
                        throw std::runtime_error(processed.error);
                    if (processed.latency) {
                        benchmarks::LatencySample sample{processed.latency->count(),
                                                         processed.snapshots != 0};
                        all.push_back(sample.nanoseconds);
                        (sample.snapshot ? snapshots : updates).push_back(sample.nanoseconds);
                    }
                }
                auto elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                queue.close();
                {
                    std::unique_lock lock(shared.mutex);
                    shared.changed.wait(lock, [&] { return shared.writer_done; });
                    if (shared.status.fatal_error) {
                        book.invalidate();
                        throw std::runtime_error(*shared.status.fatal_error);
                    }
                }
                json contents = {{"bids", json::array()},
                                 {"asks", json::array()},
                                 {"state", static_cast<int>(book.state())},
                                 {"book_sequence", optional_json(book.last_sequence())}};
                for (auto [p, q] : book.bids())
                    contents["bids"].push_back({p.raw(), q.raw()});
                for (auto [p, q] : book.asks())
                    contents["asks"].push_back({p.raw(), q.raw()});
                auto best = [](auto level) {
                    return level ? json(level->price.raw()) : json(nullptr);
                };
                result = {{"input_messages", data.size()},
                          {"book_messages", all.size()},
                          {"snapshots", snapshots.size()},
                          {"updates", updates.size()},
                          {"outcomes", outcomes},
                          {"ignored", outcomes["Ignored"]},
                          {"failed", outcomes["ParseError"].get<std::size_t>() +
                                         outcomes["SequenceGap"].get<std::size_t>() +
                                         outcomes["ApplyError"].get<std::size_t>()},
                          {"messages_per_second", data.size() / elapsed},
                          {"snapshot_latency", stats_json(snapshots)},
                          {"update_latency", stats_json(updates)},
                          {"all_book_latency", stats_json(all)},
                          {"final_sequence", optional_json(sequence.last())},
                          {"final_best_bid", best(book.best_bid())},
                          {"final_best_ask", best(book.best_ask())},
                          {"final_book_contents", contents},
                          {"final_book_hash", sha256(contents.dump())}};
            } catch (...) {
                failure = std::current_exception();
            }
            queue.close();
        });
        market.join();
    } catch (...) {
        queue.close();
        writer.join();
        throw;
    }
    writer.join();
    auto status = shared.copy_status();
    if (status.fatal_error)
        throw std::runtime_error(*status.fatal_error + " " +
                                 pipeline::unpersisted_processed_range(status));
    if (failure)
        std::rethrow_exception(failure);
    if (status.enqueued_messages != data.size() || status.written_messages != data.size() ||
        status.progress.last_received_index != status.progress.last_written_index ||
        status.progress.last_processed_index != status.progress.last_written_index)
        throw std::runtime_error("recording progress mismatch");
    recording::RawEventReader reader{output};
    std::size_t count = 0;
    while (auto written = reader.next()) {
        if (count >= data.size() || written->record_index != count ||
            written->payload != data[count].payload ||
            written->connection_id != data[count].connection_id ||
            written->venue != data[count].venue)
            throw std::runtime_error("recording FIFO/content mismatch");
        ++count;
    }
    if (reader.has_error() || count != data.size())
        throw std::runtime_error("written recording verification failed");
    result["enqueued_records"] = status.enqueued_messages;
    result["written_records"] = status.written_messages;
    result["last_received_index"] = optional_json(status.progress.last_received_index);
    result["last_enqueued_index"] = optional_json(status.progress.last_enqueued_index);
    result["last_written_index"] = optional_json(status.progress.last_written_index);
    result["last_processed_index"] = optional_json(status.progress.last_processed_index);
    return result;
}
json identity(json result) {
    result.erase("messages_per_second");
    for (auto category : {"snapshot_latency", "update_latency", "all_book_latency"}) {
        auto samples = result[category]["samples"];
        result[category] = {{"samples", samples}};
    }
    return result;
}
std::size_t positive(const std::string& value) {
    std::size_t consumed = 0;
    if (value.empty() || value.front() == '-')
        throw std::invalid_argument("expected positive integer");
    const auto n = std::stoull(value, &consumed);
    if (!n || consumed != value.size() || n > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("expected positive integer");
    return n;
}
} // namespace
int main(int argc, char** argv) {
    try {
        std::string input, output;
        std::size_t trials = 5, warmups = 1;
        recording::RawQueueConfig config;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc)
                throw std::invalid_argument("option requires a value");
            std::string option = argv[i], value = argv[i + 1];
            if (option == "--input")
                input = value;
            else if (option == "--output")
                output = value;
            else if (option == "--trials")
                trials = positive(value);
            else if (option == "--warmup-trials")
                warmups = positive(value);
            else if (option == "--queue-messages")
                config.maximum_messages = positive(value);
            else if (option == "--queue-bytes")
                config.maximum_bytes = positive(value);
            else
                throw std::invalid_argument("unknown option " + option);
        }
        if (input.empty() || output.empty())
            throw std::invalid_argument("--input and --output are required");
        if (std::filesystem::exists(output))
            throw std::invalid_argument("output must be a new file");
        if (std::string(BENCHMARK_BUILD_TYPE) != "Release")
            throw std::runtime_error("benchmark requires a Release build");
        const auto source_hash = sha256(read_bytes(input));
        std::vector<recording::RawMessage> data;
        recording::RawEventReader reader{input};
        while (auto raw = reader.next()) {
            if (raw->venue != core::Venue::Coinbase ||
                (!data.empty() && raw->connection_id < data.back().connection_id))
                throw std::runtime_error("expected ordered Coinbase recording");
            data.push_back(std::move(*raw));
        }
        if (reader.has_error())
            throw std::runtime_error(std::string(reader.error()));
        if (data.empty())
            throw std::runtime_error("dataset is empty");
        if (source_hash != sha256(read_bytes(input)))
            throw std::runtime_error("input changed while loading");
        json report = {{"source", input},
                       {"source_sha256", source_hash},
                       {"build_type", BENCHMARK_BUILD_TYPE},
                       {"platform", BENCHMARK_PLATFORM},
                       {"warmup_trials", warmups},
                       {"queue_maximum_messages", config.maximum_messages},
                       {"queue_maximum_bytes", config.maximum_bytes},
                       {"percentile_rule", "nearest rank: ceil(percentile * sample_count) - 1"},
                       {"trials", json::array()}};
        json expected;
        for (std::size_t i = 0; i < warmups + trials; ++i) {
            auto result = trial(data, config);
            const auto stable = identity(result);
            if (i == 0)
                expected = stable;
            else if (stable != expected)
                throw std::runtime_error("nondeterministic trial counts or final book");
            if (i >= warmups) {
                result["trial"] = i - warmups + 1;
                report["trials"].push_back(std::move(result));
            }
        }
        std::ofstream out(output, std::ios::binary);
        out << report.dump(2) << '\n';
        out.close();
        if (!out)
            throw std::runtime_error("report write failed");
        std::cout << "Verified " << trials << " trials; source_sha256=" << source_hash
                  << "; report=" << output << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
