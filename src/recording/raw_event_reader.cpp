#include "recording/raw_event_reader.hpp"

#include <nlohmann/json.hpp>

#include <sstream>

using nlohmann::json;

namespace recording {

RawEventReader::RawEventReader(const std::filesystem::path& input_file)
    : input_(input_file, std::ios::in | std::ios::binary) {
    if (!input_.is_open()) {
        last_error_ = "failed to open input file";
    }
}

std::optional<RawMessage> RawEventReader::next() {
    if (!input_.is_open() || !input_.good()) {
        if (last_error_.empty()) last_error_ = "input stream not open";
        return std::nullopt;
    }

    std::string line;
    if (!std::getline(input_, line)) {
        if (input_.eof()) return std::nullopt;
        last_error_ = "failed to read line";
        return std::nullopt;
    }

    // parse JSON
    json j;
    try {
        j = json::parse(line);
    } catch (const std::exception& ex) {
        last_error_ = std::string("json parse error: ") + ex.what();
        return std::nullopt;
    }

    // Validate format_version
    if (!j.contains("format_version") || j["format_version"].get<int>() != 1) {
        last_error_ = "unsupported or missing format_version";
        return std::nullopt;
    }

    // Required fields
    const std::vector<std::string> required = {
        "record_index", "venue", "connection_id", "receive_wall_ns", "receive_monotonic_ns", "payload"
    };
    for (const auto& f : required) {
        if (!j.contains(f)) {
            last_error_ = std::string("missing field: ") + f;
            return std::nullopt;
        }
    }

    std::uint64_t record_index = j["record_index"].get<std::uint64_t>();
    // contiguous index check
    if (record_index != expected_index_) {
        std::ostringstream oss;
        oss << "noncontiguous record index: expected " << expected_index_ << " got " << record_index;
        last_error_ = oss.str();
        return std::nullopt;
    }

    std::string venue_s = j["venue"].get<std::string>();
    Venue venue = Venue::Coinbase;
    if (venue_s == "coinbase") venue = Venue::Coinbase;
    else if (venue_s == "kraken") venue = Venue::Kraken;
    else {
        last_error_ = "unknown venue";
        return std::nullopt;
    }

    RawMessage msg;
    msg.record_index = record_index;
    msg.venue = venue;
    msg.connection_id = j["connection_id"].get<std::uint64_t>();
    msg.receive_wall_time.nanoseconds = j["receive_wall_ns"].get<std::int64_t>();
    msg.receive_monotonic_time.nanoseconds = j["receive_monotonic_ns"].get<std::int64_t>();
    msg.payload = j["payload"].get<std::string>();

    ++expected_index_;
    return msg;
}

bool RawEventReader::has_error() const { return !last_error_.empty(); }

std::string_view RawEventReader::error() const { return std::string_view(last_error_); }

} // namespace recording
