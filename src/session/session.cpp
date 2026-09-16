#include "session/session.hpp"
#include "recording/raw_event_reader.hpp"
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <set>
#include <sstream>
#include <unistd.h>
#ifndef __APPLE__
#include <sys/syscall.h>
#endif
using nlohmann::json;
namespace sessions {
namespace {
std::string utc_now() {
    auto now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
struct Digest {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    Digest() {
        if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("SHA-256 initialization failed");
    }
    ~Digest() { EVP_MD_CTX_free(ctx); }
    void add(std::string_view bytes) {
        if (EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) != 1)
            throw std::runtime_error("SHA-256 update failed");
    }
    std::string finish() {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int size;
        if (EVP_DigestFinal_ex(ctx, result, &size) != 1)
            throw std::runtime_error("SHA-256 finalization failed");
        std::ostringstream out;
        for (unsigned i = 0; i < size; ++i)
            out << std::hex << std::setw(2) << std::setfill('0') << unsigned(result[i]);
        return out.str();
    }
};
// A private temporary file is published with an atomic, no-replace rename.
// Readers see either no destination or the entire closed document.
void publish(const std::filesystem::path& path, const json& value) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto pattern = path.string() + ".tmp-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    int fd = mkstemp(name.data());
    if (fd < 0)
        throw std::runtime_error("cannot create temporary manifest/result");
    auto bytes = value.dump(2) + "\n";
    bool ok = true;
    for (std::size_t offset = 0; offset < bytes.size();) {
        auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(fd) != 0)
        ok = false;
    if (::close(fd) != 0)
        ok = false;
    if (ok) {
#ifdef __APPLE__
        ok = ::renamex_np(name.data(), path.c_str(), RENAME_EXCL) == 0;
#else
        ok = ::syscall(SYS_renameat2, AT_FDCWD, name.data(), AT_FDCWD, path.c_str(),
                       1 /* RENAME_NOREPLACE */) == 0;
#endif
    }
    ::unlink(name.data());
    if (!ok)
        throw std::runtime_error("cannot atomically publish new file: " + path.string());
}
template <class T> json optional(T value) { return value ? json(*value) : json(nullptr); }
const char* book_name(core::BookState state) {
    switch (state) {
    case core::BookState::Initializing:
        return "INITIALIZING";
    case core::BookState::Valid:
        return "VALID";
    case core::BookState::Stale:
        return "STALE";
    case core::BookState::Resyncing:
        return "RESYNCING";
    case core::BookState::Disconnected:
        return "DISCONNECTED";
    case core::BookState::Invalid:
        return "INVALID";
    }
    throw std::runtime_error("unknown book state");
}
struct Scan {
    std::uint64_t records = 0;
    std::set<std::uint64_t> connections;
};
Scan scan(const std::filesystem::path& raw) {
    Scan result;
    recording::RawEventReader reader(raw);
    while (auto e = reader.next()) {
        if (e->venue != core::Venue::Coinbase)
            throw std::runtime_error("unexpected venue");
        ++result.records;
        result.connections.insert(e->connection_id);
    }
    if (reader.has_error())
        throw std::runtime_error(std::string(reader.error()));
    return result;
}
} // namespace
std::string sha256(std::string_view bytes) {
    Digest d;
    d.add(bytes);
    return d.finish();
}
std::string file_sha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("missing raw file: " + path.string());
    Digest d;
    char buffer[65536];
    while (input.read(buffer, sizeof(buffer)) || input.gcount())
        d.add({buffer, static_cast<std::size_t>(input.gcount())});
    if (!input.eof())
        throw std::runtime_error("raw file read failed");
    return d.finish();
}
json configuration() {
    return {
        {"venues", {"coinbase"}},
        {"instruments", {"BTC-USD"}},
        {"product_metadata", {{"coinbase:BTC-USD", {{"price_scale", 2}, {"quantity_scale", 8}}}}},
        {"processor_version", 1}};
}
Capture::Capture(const std::filesystem::path& directory) : directory_(directory) {
    if (!directory.parent_path().empty())
        std::filesystem::create_directories(directory.parent_path());
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("session directory must be new");
    std::filesystem::create_directory(directory / "raw");
    manifest_ = configuration();
    manifest_["format_version"] = 1;
    manifest_["session_id"] = directory.filename().string();
    manifest_["status"] = "in_progress";
    manifest_["started_at_utc"] = utc_now();
    manifest_["application"] = {{"git_commit", SESSION_GIT_COMMIT},
                                {"build_type", SESSION_BUILD_TYPE}};
    manifest_["platform"] = {{"os", SESSION_OS}, {"architecture", SESSION_ARCH}};
    manifest_["configuration_hash"] = sha256(configuration().dump());
    manifest_["raw_files"] = json::array({{{"venue", "coinbase"}, {"path", "raw/coinbase.jsonl"}}});
    publish(directory_ / "manifest.inprogress.json", manifest_);
}
std::filesystem::path Capture::raw_path() const { return directory_ / "raw/coinbase.jsonl"; }
void Capture::finalize(const pipeline::RuntimeStatus& s) {
    using pipeline::RuntimeState;
    if (s.runtime_state == RuntimeState::Starting || s.runtime_state == RuntimeState::Running ||
        s.runtime_state == RuntimeState::Stopping)
        throw std::runtime_error("cannot finalize running recorder");
    const char* status = "interrupted";
    const char* runtime = "Stopped";
    switch (s.runtime_state) {
    case RuntimeState::Stopped:
        status = "complete";
        break;
    case RuntimeState::StoppedRecordingError:
        status = "recording_failure";
        runtime = "StoppedRecordingError";
        break;
    case RuntimeState::StoppedQueueFull:
        status = "queue_overflow";
        runtime = "StoppedQueueFull";
        break;
    case RuntimeState::StoppedProcessingError:
        status = "processing_failure";
        runtime = "StoppedProcessingError";
        break;
    default:
        break;
    }
    manifest_["lifecycle_events"] = json::array();
    for (const auto& event : s.lifecycle_events)
        manifest_["lifecycle_events"].push_back({{"before_record_index", event.before_record_index},
                                                 {"state", book_name(event.state)},
                                                 {"reason", event.reason}});
    manifest_["status"] = status;
    manifest_["final_runtime_state"] = runtime;
    manifest_["ended_at_utc"] = utc_now();
    manifest_["stop_reason"] = s.fatal_error.value_or("requested_shutdown");
    manifest_["last_received_index"] = optional(s.progress.last_received_index);
    manifest_["last_enqueued_index"] = optional(s.progress.last_enqueued_index);
    manifest_["last_written_index"] = optional(s.progress.last_written_index);
    manifest_["last_processed_index"] = optional(s.progress.last_processed_index);
    manifest_["unpersisted_records"] = s.received_messages - s.written_messages;
    manifest_["unpersisted_processed_tail"] = !pipeline::unpersisted_processed_range(s).empty();
    auto& raw = manifest_["raw_files"][0];
    if (std::filesystem::exists(raw_path())) {
        raw["bytes"] = std::filesystem::file_size(raw_path());
        raw["sha256"] = file_sha256(raw_path());
        raw["records"] = s.written_messages;
        raw["first_record_index"] = s.written_messages ? json(0) : json(nullptr);
        raw["last_record_index"] = optional(s.progress.last_written_index);
        try {
            auto scanned = scan(raw_path());
            manifest_["connections"] = scanned.connections.size();
            if (scanned.records != s.written_messages)
                manifest_["status"] = "recording_failure";
        } catch (...) {
            manifest_["status"] = "recording_failure";
        }
    } else
        manifest_["connections"] = 0;
    if (manifest_["status"] == "complete" &&
        (s.fatal_error || s.received_messages != s.written_messages))
        manifest_["status"] = "recording_failure";
    publish(directory_ / "manifest.json", manifest_);
    std::filesystem::remove(directory_ / "manifest.inprogress.json");
}
int replay(const std::filesystem::path& directory, const std::filesystem::path& output,
           bool allow_incomplete, bool include_final_book) {
    auto manifest_path = directory / "manifest.json";
    if (!std::filesystem::exists(manifest_path) && allow_incomplete)
        manifest_path = directory / "manifest.inprogress.json";
    std::ifstream input(manifest_path);
    if (!input)
        throw std::runtime_error("missing session manifest");
    const std::string manifest_bytes{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
    const auto manifest_checksum = sha256(manifest_bytes);
    json manifest = json::parse(manifest_bytes);
    if (!manifest.at("format_version").is_number_unsigned() || manifest.at("format_version") != 1)
        throw std::runtime_error("unsupported manifest version");
    const std::set<std::string> statuses{"in_progress",        "complete",
                                         "recording_failure",  "queue_overflow",
                                         "processing_failure", "interrupted"};
    if (!statuses.contains(manifest.at("status").get<std::string>()))
        throw std::runtime_error("unknown session status");
    bool complete = manifest.at("status") == "complete";
    if (!complete && !allow_incomplete)
        throw std::runtime_error("incomplete session requires --allow-incomplete");
    auto config = configuration();
    auto recorded_products = manifest.at("product_metadata");
    if (!recorded_products.is_object() || recorded_products.size() != 1 ||
        !recorded_products.contains("coinbase:BTC-USD"))
        throw std::runtime_error("unexpected product metadata");
    const auto& recorded_precision = recorded_products.at("coinbase:BTC-USD");
    if (!recorded_precision.is_object() || recorded_precision.size() != 2)
        throw std::runtime_error("invalid precision metadata");
    for (auto key : {"price_scale", "quantity_scale"})
        if (!recorded_precision.at(key).is_number_unsigned() ||
            recorded_precision.at(key).get<std::uint64_t>() > 18)
            throw std::runtime_error("unsupported precision scale");
    config["product_metadata"] = recorded_products;
    for (auto key : {"venues", "instruments", "product_metadata", "processor_version"})
        if (manifest.at(key) != config.at(key))
            throw std::runtime_error(
                "unexpected venue, instrument, precision or processor configuration");
    if (manifest.at("configuration_hash") != sha256(config.dump()))
        throw std::runtime_error("configuration hash mismatch");
    if (manifest.at("raw_files").size() != 1)
        throw std::runtime_error("expected one Coinbase raw file");
    const auto& metadata = manifest.at("raw_files").at(0);
    if (metadata.at("venue") != "coinbase")
        throw std::runtime_error("unexpected raw venue");
    std::filesystem::path relative = metadata.at("path").get<std::string>();
    if (relative.is_absolute())
        throw std::runtime_error("raw path must be relative");
    auto root = std::filesystem::canonical(directory);
    auto raw = std::filesystem::canonical(root / relative);
    auto inside = raw.lexically_relative(root);
    if (inside.empty() || *inside.begin() == "..")
        throw std::runtime_error("raw path escapes session");
    auto checksum = file_sha256(raw);
    for (auto key : {"bytes", "records"})
        if ((complete || metadata.contains(key)) && !metadata.at(key).is_number_unsigned())
            throw std::runtime_error("invalid raw metadata count");
    if ((complete || metadata.contains("bytes")) &&
        metadata.at("bytes") != std::filesystem::file_size(raw))
        throw std::runtime_error("raw file size mismatch");
    if ((complete || metadata.contains("sha256")) && metadata.at("sha256") != checksum)
        throw std::runtime_error("raw checksum mismatch");
    auto scanned = scan(raw); // Full validation precedes processing, including all record indexes.
    if ((complete || metadata.contains("records")) && metadata.at("records") != scanned.records)
        throw std::runtime_error("raw record count mismatch");
    if (complete) {
        const auto last_index = scanned.records ? json(scanned.records - 1) : json(nullptr);
        for (auto key : {"last_received_index", "last_enqueued_index", "last_written_index",
                         "last_processed_index"})
            if (manifest.at(key) != last_index)
                throw std::runtime_error("inconsistent complete runtime progress");
        if (manifest.at("final_runtime_state") != "Stopped")
            throw std::runtime_error("complete session has non-stopped runtime");
        if (metadata.at("first_record_index") != (scanned.records ? json(0) : json(nullptr)) ||
            metadata.at("last_record_index") !=
                (scanned.records ? json(scanned.records - 1) : json(nullptr)) ||
            manifest.at("unpersisted_records") != 0 ||
            manifest.at("unpersisted_processed_tail") != false)
            throw std::runtime_error("inconsistent complete manifest");
    }
    std::uint64_t last_lifecycle_index = 0;
    for (const auto& event : manifest.value("lifecycle_events", json::array())) {
        if (!event.at("before_record_index").is_number_unsigned())
            throw std::runtime_error("invalid lifecycle index");
        auto index = event.at("before_record_index").get<std::uint64_t>();
        const std::set<std::string> states{"DISCONNECTED", "RESYNCING", "INITIALIZING", "STALE",
                                           "INVALID"};
        if (index < last_lifecycle_index || (complete && index > scanned.records) ||
            !states.contains(event.at("state").get<std::string>()) ||
            !event.at("reason").is_string())
            throw std::runtime_error("invalid lifecycle event");
        last_lifecycle_index = index;
    }
    core::OrderBook book(core::Venue::Coinbase, core::Instrument::BTC_USD);
    core::SequenceTracker sequence;
    adapters::coinbase::FeedHealth health;
    core::ReplayClock clock;
    auto precision = manifest.at("product_metadata").at("coinbase:BTC-USD");
    adapters::coinbase::CoinbaseSymbolMapper symbols(
        {{"BTC-USD",
          {core::Instrument::BTC_USD, precision.at("price_scale").get<std::uint8_t>(),
           precision.at("quantity_scale").get<std::uint8_t>()}}});
    adapters::coinbase::CoinbaseMessageProcessor processor(book, sequence, health, clock,
                                                           std::move(symbols));
    json outcomes = json::object(), transitions = json::array();
    for (int i = 0; i <= int(adapters::coinbase::MessageOutcome::ApplyError); ++i)
        outcomes[adapters::coinbase::outcome_name(
            static_cast<adapters::coinbase::MessageOutcome>(i))] = 0;
    std::optional<std::uint64_t> connection;
    std::uint64_t boundaries = 0, snapshots = 0, updates = 0, count = 0;
    auto transition = [&](std::uint64_t index, core::BookState from, core::BookState to,
                          const std::string& reason) {
        if (from != to)
            transitions.push_back({{"record_index", index},
                                   {"from", book_name(from)},
                                   {"to", book_name(to)},
                                   {"reason", reason}});
    };
    const auto lifecycle = manifest.value("lifecycle_events", json::array());
    std::size_t lifecycle_index = 0;
    bool lifecycle_connected = false;
    if (!lifecycle.empty())
        book.mark_disconnected();
    auto apply_lifecycle = [&](std::uint64_t index) {
        lifecycle_connected = false;
        while (lifecycle_index < lifecycle.size() &&
               lifecycle[lifecycle_index].at("before_record_index") == index) {
            const auto& event = lifecycle[lifecycle_index++];
            auto previous = book.state();
            const auto state = event.at("state").get<std::string>();
            if (state == "DISCONNECTED")
                book.mark_disconnected();
            else if (state == "RESYNCING")
                book.mark_resyncing();
            else if (state == "INITIALIZING") {
                book.mark_initializing();
                lifecycle_connected = true;
            } else if (state == "STALE")
                book.mark_stale();
            else if (state == "INVALID")
                book.invalidate();
            else
                throw std::runtime_error("invalid lifecycle state");
            sequence.reset();
            transition(index, previous, book.state(), event.at("reason").get<std::string>());
        }
    };
    recording::RawEventReader reader(raw);
    while (auto e = reader.next()) {
        apply_lifecycle(e->record_index);
        clock.set(e->receive_wall_time, e->receive_monotonic_time);
        if (connection != e->connection_id) {
            if (connection)
                ++boundaries;
            if (connection && !lifecycle_connected) {
                auto previous = book.state();
                book.mark_disconnected();
                transition(e->record_index, previous, book.state(), "connection_id_changed");
            }
            if (!lifecycle_connected) {
                auto previous = book.state();
                book.mark_resyncing();
                transition(e->record_index, previous, book.state(), "resynchronization");
                previous = book.state();
                book.mark_initializing();
                transition(e->record_index, previous, book.state(), "new_connection");
            }
            sequence.reset();
            health.reset(clock.monotonic_now());
            connection = e->connection_id;
        }
        auto previous = book.state();
        auto result = processor.process(*e);
        transition(e->record_index, previous, book.state(),
                   result.error.empty()
                       ? (snapshots ? "fresh_recovery_snapshot" : "initial_snapshot")
                       : result.error);
        ++count;
        outcomes[adapters::coinbase::outcome_name(result.outcome)] =
            outcomes[adapters::coinbase::outcome_name(result.outcome)].get<std::uint64_t>() + 1;
        snapshots += result.snapshots;
        updates += result.updates;
    }
    apply_lifecycle(count);
    if (complete && lifecycle_index != lifecycle.size())
        throw std::runtime_error("invalid lifecycle ordering");
    if (!complete && manifest.at("status") != "in_progress") {
        auto previous = book.state();
        book.invalidate();
        sequence.reset();
        transition(count, previous, book.state(), "terminal_failure");
    }
    if (reader.has_error() || count != scanned.records || file_sha256(raw) != checksum)
        throw std::runtime_error("source changed during replay");
    json contents = {{"bids", json::array()},
                     {"asks", json::array()},
                     {"state", book_name(book.state())},
                     {"book_sequence", optional(book.last_sequence())}};
    for (auto [p, q] : book.bids())
        contents["bids"].push_back({p.raw(), q.raw()});
    for (auto [p, q] : book.asks())
        contents["asks"].push_back({p.raw(), q.raw()});
    auto best = [](auto level) { return level ? json(level->price.raw()) : json(nullptr); };
    json result = {{"format_version", 1},
                   {"source_session_id", manifest.at("session_id")},
                   {"source_manifest_sha256", manifest_checksum},
                   {"source_raw_sha256", checksum},
                   {"configuration_hash", manifest.at("configuration_hash")},
                   {"complete_source", complete},
                   {"research_valid", complete},
                   {"messages_read", count},
                   {"outcomes", outcomes},
                   {"book_messages", outcomes["BookUpdated"]},
                   {"ignored_messages", outcomes["Ignored"]},
                   {"snapshots", snapshots},
                   {"updates", updates},
                   {"sequence_gaps", outcomes["SequenceGap"]},
                   {"connection_boundaries", boundaries},
                   {"final_sequence", optional(sequence.last())},
                   {"final_book_state", book_name(book.state())},
                   {"final_best_bid", best(book.best_bid())},
                   {"final_best_ask", best(book.best_ask())},
                   {"final_bid_levels", book.bids().size()},
                   {"final_ask_levels", book.asks().size()},
                   {"final_book_hash", sha256(contents.dump())},
                   {"state_transitions", transitions},
                   {"state_transition_hash", sha256(transitions.dump())}};
    auto canonical = result;
    canonical.erase("source_session_id");
    canonical.erase("source_manifest_sha256");
    canonical["final_book"] = contents;
    result["deterministic_result_hash"] = sha256(canonical.dump());
    if (include_final_book)
        result["final_book"] = contents;
    if (file_sha256(manifest_path) != manifest_checksum)
        throw std::runtime_error("manifest changed during replay");
    publish(output, result);
    return 0;
}
} // namespace sessions
